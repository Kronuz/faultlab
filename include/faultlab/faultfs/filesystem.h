// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs's model, class filesystem: the inodes, the current and durable namespaces, the
// namespace log and ordering points, fault rules, the capacity, and crash()'s rebuild; with the
// test and recorder access to it and the inspection of an inode.
#pragma once

// 1 (the tests) checks the reference counts and used() against a full recount at every collection.
#ifndef FAULTLAB_CHECK_COUNTS
#define FAULTLAB_CHECK_COUNTS 0
#endif

#include "completion/io.h"
#include "faultlab/faultfs/types.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kronuz::faultlab {

// The model one or more IO instances share; it outlives every handle.
class filesystem {
public:
	explicit filesystem(std::uint64_t capacity = ~std::uint64_t(0)) : capacity_(capacity) {
		const char* m = std::getenv("FAULTLAB_MAP");
		map_files_ = m && *m == '1';
	}
	filesystem(const filesystem&) = delete;
	filesystem& operator=(const filesystem&) = delete;
	filesystem& operator=(filesystem&&) = delete;
	// Moves the model; only a filesystem no io, handle or mapping refers to.
	filesystem(filesystem&& other) noexcept {
		std::lock_guard lock(other.mu_);
		take(std::move(other));
	}

	// A directory at `path` ("a" or "a/b"), whose parent exists: durable at once.
	void make_directory(std::string_view path) {
		std::lock_guard lock(mu_);
		fold_locked();
		std::string_view rest = path;
		for (;;) {
			const auto slash = rest.find('/');
			detail::check_name(rest.substr(0, slash));
			if (slash == std::string_view::npos) { break; }
			rest.remove_prefix(slash + 1);
		}
		const std::string p(path);
		if (!directory_exists(detail::parent(p))) { detail::fail(ENOENT, "faultfs make_directory"); }
		if (directories_.count(p) || names_.count(p)) { detail::fail(EEXIST, "faultfs make_directory"); }
		directories_.insert(p);
	}

	void add_fault(const fault& f) {
		std::lock_guard lock(mu_);
		rules_.push_back({f, 0});
	}
	void clear_faults() {
		std::lock_guard lock(mu_);
		rules_.clear();
		media_.clear();
	}
	// Reads of the file at `path` overlapping [begin, end) fail or return a flipped bit (media
	// above), `times` times, or always when 0.
	void add_media_fault(const std::string& path, media kind, std::uint64_t begin, std::uint64_t end, std::uint64_t times = 0) {
		std::lock_guard lock(mu_);
		auto it = names_.find(path);
		if (it == names_.end()) { detail::fail(ENOENT, "faultfs media fault"); }
		media_.push_back({inodes_.at(it->second), kind, begin, end, times});
	}
	// What io::directory_filesystem reports (default: a local filesystem named faultfs).
	void classify(journal::FilesystemClass fs) {
		std::lock_guard lock(mu_);
		class_ = std::move(fs);
	}
	// What io::directory_trust reports (default: owned and private).
	void set_trust(journal::DirectoryTrust trust) {
		std::lock_guard lock(mu_);
		trust_ = trust;
	}
	// Reads that met a media fault so far.
	std::uint64_t media_hits() const {
		std::lock_guard lock(mu_);
		return media_hits_;
	}
	// Whether File::map maps (default: FAULTLAB_MAP=1). Off, it returns null, so callers read and
	// media faults apply; on, truncating a file below a live mapping aborts, as a real mapping's
	// reader would fault.
	void map_files(bool on) {
		std::lock_guard lock(mu_);
		map_files_ = on;
	}
	// Each write then stores at most `max_bytes` (0: no limit) and returns the count.
	void short_writes(std::size_t max_bytes) {
		std::lock_guard lock(mu_);
		short_ = max_bytes;
	}
	// Silent write damage: each of the next `count` writes of at least `min` bytes (and more than
	// `at`) stores one flipped bit `at` bytes in and succeeds, as a corrupted device write.
	void damage_writes(std::size_t min, std::size_t at, int count = 1) {
		std::lock_guard lock(mu_);
		damage_min_ = min;
		damage_at_ = at;
		damage_left_ = count;
	}
	// Writes damaged so far.
	int damaged_writes() const {
		std::lock_guard lock(mu_);
		return damaged_;
	}
	// Power goes off after `n` more mutating operations (writes, truncates, preallocations, syncs,
	// creates, links, renames, unlinks): each later one fails with EIO before its effect, until crash().
	void power_off_after(std::uint64_t n) {
		std::lock_guard lock(mu_);
		fold_locked();
		power_left_ = n;
	}
	// Whether power_off_after has run out: every mutating operation now fails, until crash().
	bool powered_off() const {
		std::lock_guard lock(mu_);
		return power_left_ && *power_left_ == 0;
	}
	// Ends power_off_after's cut as a process crash, where crash() ends it as a power loss: the
	// process dies as at kill(), every byte stays, synced or not, and operations succeed again.
	void power_on() {
		std::lock_guard lock(mu_);
		fold_locked();
		release();
		power_left_.reset();
	}
	// Mutating operations so far, to enumerate crash points.
	std::uint64_t mutations() const {
		std::lock_guard lock(mu_);
		return mutations_;
	}
	// Syncs so far: file syncs, a staged file's fsync at an ordering point, directory syncs and
	// ordering points. A path that syncs nothing leaves it unchanged.
	std::uint64_t syncs() const {
		std::lock_guard lock(mu_);
		return syncs_;
	}
	// Operations of `kind` so far, failed ones included: mutations() by kind, and try_lease calls.
	std::uint64_t operations(op kind) const {
		std::lock_guard lock(mu_);
		return ops_[std::size_t(kind)];
	}
	// Every call into an IO or File so far, reads, opens, maps and queries included (not a
	// mapping's or lock's release): a code path that makes no device call leaves it unchanged.
	std::uint64_t calls() const {
		std::lock_guard lock(mu_);
		return calls_;
	}
	// Each File::sync then waits at least `d` before its effect, as a device flush does.
	void sync_latency(std::chrono::microseconds d) noexcept { sync_latency_.store(d.count(), std::memory_order_relaxed); }
	void set_capacity(std::uint64_t bytes) {
		std::lock_guard lock(mu_);
		capacity_ = bytes;
	}

	// Power loss: rebuilds the state storage could hold, chosen by `seed` and `policy`; every open
	// handle becomes stale, owner locks and leases are released and shared mappings detach.
	void crash(std::uint64_t seed, crash_policy policy = {}) {
		std::lock_guard lock(mu_);
		rebuild(seed, forced(policy), policy.size == size_pick::sector);
	}
	// Process death: handles go stale, owner locks and leases release and shared mappings detach
	// (their stores kept), as at crash(); current bytes, durable bytes, the namespaces and the
	// staged ordering epochs all stay, so a later crash() still loses the unsynced part.
	void kill() {
		std::lock_guard lock(mu_);
		fold_locked();
		release();
	}
	// A deep copy of the model, with no handles, locks, leases or mappings: its stores folded first.
	filesystem snapshot() const {
		std::lock_guard lock(mu_);
		fold_locked();
		filesystem copy;
		copy.copy_from(*this);
		return copy;
	}

	// The current bytes of the file at `path`, for readers that take a view (valid until the next
	// change to that file or a crash).
	std::string_view view(std::string_view path) const {
		std::lock_guard lock(mu_);
		fold_locked();
		auto it = names_.find(std::string(path));
		if (it == names_.end()) { detail::fail(ENOENT, "faultfs view"); }
		return inodes_.at(it->second)->current;
	}
	// The names in `directory` (its files and subdirectories), sorted.
	std::vector<std::string> list(std::string_view directory = {}) const {
		std::lock_guard lock(mu_);
		fold_locked();
		if (!directory_exists(directory)) { detail::fail(ENOENT, "faultfs list"); }
		return entries(directory);
	}
	std::uint64_t used() const {
		std::lock_guard lock(mu_);
		fold_locked();
		return used_locked();
	}

private:
	friend class io;
	friend class file;
	friend struct detail::crash_access;
	friend struct detail::model_access;

	using name_map = std::map<std::string, std::uint64_t>;
	struct logged {
		op kind;
		std::string a, b;
		std::uint64_t id;
	};
	struct rule {
		fault f;
		std::uint64_t seen;
	};

	// With the power off, an operation fails having done nothing: what storage keeps is the crash's.
	static inline const fault power_off_{op::write, 1, EIO, false};
	// Counts an operation against the rules; returns the fault that fires, if any. With the power
	// off, every mutating operation fails before its effect.
	const fault* check(op kind) {
		++mutations_;
		++ops_[std::size_t(kind)];
		if (kind == op::sync || kind == op::sync_directory) { ++syncs_; }
		if (power_left_) {
			if (*power_left_ == 0) { return &power_off_; }
			--*power_left_;
		}
		return rule_for(kind);
	}
	// The rule that fires for a non-mutating operation (a lease), which the power never fails.
	const fault* rule_for(op kind) {
		for (auto& r : rules_) {
			if (r.f.kind == kind && ++r.seen == r.f.nth) { return &r.f; }
		}
		return nullptr;
	}
	std::uint64_t used_locked() const {
		check_counts();
		return used_;
	}
	bool directory_exists(std::string_view path) const { return path.empty() || directories_.count(std::string(path)) != 0; }
	std::vector<std::string> entries(std::string_view directory) const {
		std::vector<std::string> out;
		for (const auto& [n, _] : names_) {
			if (detail::parent(n) == directory) { out.push_back(detail::base(n)); }
		}
		for (const auto& d : directories_) {
			if (detail::parent(d) == directory) { out.push_back(detail::base(d)); }
		}
		std::sort(out.begin(), out.end());
		return out;
	}
	std::uint64_t links_of(std::uint64_t id) const {
		const auto it = inodes_.find(id);
		return it == inodes_.end() ? 0 : it->second->links;
	}
	// Reference counts: each change to a namespace or the log counts the inodes it names, so an
	// operation costs what it touches. An inode whose refs fall to zero waits in unreferenced_.
	void ref(std::uint64_t id, bool link) {
		auto& node = *inodes_.at(id);
		++node.refs;
		node.links += link;
	}
	void unref(std::uint64_t id, bool link) {
		auto& node = *inodes_.at(id);
		node.links -= link;
		if (--node.refs == 0) { unreferenced_.insert(id); }
	}
	void set_name(name_map& names, const std::string& path, std::uint64_t id) {
		const bool link = &names == &names_;
		auto [it, fresh] = names.try_emplace(path, id);
		if (!fresh) {
			if (it->second == id) { return; }
			unref(std::exchange(it->second, id), link);
		}
		ref(id, link);
	}
	void erase_name(name_map& names, name_map::iterator it) {
		const std::uint64_t id = it->second;
		names.erase(it);
		unref(id, &names == &names_);
	}
	void log(logged e) {
		ref(e.id, false);
		log_.push_back(std::move(e));
	}
	// Brings `node`'s share of used() up to date after a call that may have resized it.
	void account(detail::inode& node) {
		used_ = used_ - node.counted + node.footprint();
		node.counted = node.footprint();
	}
	// Rebuilds every count from the namespaces, the log and the inodes (after a crash, a settled
	// ordering point or a copy, which change them wholesale).
	void recount() {
		for (auto& [id, node] : inodes_) { node->refs = node->links = 0; }
		for (const auto& [_, id] : names_) { ref(id, true); }
		for (const auto& [_, id] : durable_names_) { ref(id, false); }
		for (const auto& e : log_) { ref(e.id, false); }
		used_ = 0;
		unreferenced_.clear();
		for (auto& [id, node] : inodes_) {
			node->counted = node->footprint();
			used_ += node->counted;
			if (node->refs == 0) { unreferenced_.insert(id); }
		}
	}
	// Drops the inodes nothing references and no handle holds; one still held waits for a later call.
	void collect() {
		for (auto it = unreferenced_.begin(); it != unreferenced_.end();) {
			const auto node = inodes_.find(*it);
			const bool unreferenced = node != inodes_.end() && node->second->refs == 0;
			if (unreferenced && node->second.use_count() > 1) {
				++it;
				continue;
			}
			if (unreferenced) {
				used_ -= node->second->counted;
				inodes_.erase(node);
			}
			it = unreferenced_.erase(it);
		}
		check_counts();
	}
	// FAULTLAB_CHECK_COUNTS: every count against a full recount, and no inode staged without an
	// ordering point unless strays_ lists it (settle_directory relies on it).
	void check_counts() const {
#if FAULTLAB_CHECK_COUNTS
		std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> want;  // refs, links
		for (const auto& [_, id] : names_) { ++want[id].first, ++want[id].second; }
		for (const auto& [_, id] : durable_names_) { ++want[id].first; }
		for (const auto& e : log_) { ++want[e.id].first; }
		std::uint64_t used = 0;
		for (const auto& [id, node] : inodes_) {
			const auto w = want.count(id) ? want.at(id) : std::pair<std::uint64_t, std::uint64_t>{};
			if (node->refs != w.first || node->links != w.second) { throw std::logic_error("faultfs: inode " + std::to_string(id) + " miscounts its references"); }
			if (node->refs == 0 && !unreferenced_.count(id)) { throw std::logic_error("faultfs: unreferenced inode " + std::to_string(id) + " not collectable"); }
			if (node->counted != node->footprint()) { throw std::logic_error("faultfs: inode " + std::to_string(id) + " unaccounted in used()"); }
			if (marks_.empty() && !node->staged.empty() && !strays_.count(id)) { throw std::logic_error("faultfs: inode " + std::to_string(id) + " staged without an ordering point"); }
			used += node->footprint();
		}
		if (used != used_) { throw std::logic_error("faultfs: used() miscounted"); }
#endif
	}
	void clear_marks() {
		marks_.clear();
		for (auto& [id, node] : inodes_) { node->at_mark.clear(); }
	}
	// Applies a logged namespace op to `names`, as replay and settling do.
	static void apply(std::map<std::string, std::uint64_t>& names, const logged& e) {
		switch (e.kind) {
		case op::create:
			names.try_emplace(e.a, e.id);
			break;
		case op::replace:  // onto its own name: nothing changes
			if (auto it = names.find(e.a); e.a != e.b && it != names.end() && it->second == e.id) {
				names[e.b] = e.id;
				names.erase(e.a);
			}
			break;
		case op::link:
			if (auto it = names.find(e.a); it != names.end() && it->second == e.id) { names.try_emplace(e.b, e.id); }
			break;
		case op::remove:
			if (auto it = names.find(e.a); it != names.end() && it->second == e.id) { names.erase(it); }
			break;
		default:
			break;
		}
	}
	// What an ordering point staged becomes durable: its files' bytes and the namespace changes
	// logged before it. Later changes stay as they were, unordered.
	void settle_staged() {
		if (marks_.empty()) { return; }
		for (auto& [id, node] : inodes_) {
			if (!node->staged.empty()) { node->settle(); }
		}
		for (std::size_t i = 0; i < marks_.back(); ++i) { apply(durable_names_, log_[i]); }
		log_.erase(log_.begin(), log_.begin() + std::ptrdiff_t(marks_.back()));
		clear_marks();
		strays_.clear();
		recount();
	}
	// sync_directory: what the ordering points staged, then `directory`'s namespace, durable. Its
	// durable names differ from its current ones only at the paths its logged ops touched.
	void settle_directory(const std::string& directory) {
		settle_staged();
		for (const std::uint64_t id : strays_) {
			if (auto it = inodes_.find(id); it != inodes_.end() && !it->second->staged.empty()) { it->second->settle(); }
		}
		strays_.clear();
		std::set<std::string> touched;
		std::vector<std::uint64_t> dropped;
		std::erase_if(log_, [&](const logged& e) {
			if (detail::parent(e.a) != directory) { return false; }
			touched.insert(e.a);
			if (!e.b.empty()) { touched.insert(e.b); }
			dropped.push_back(e.id);
			return true;
		});
		for (const auto& path : touched) {
			if (const auto now = names_.find(path); now != names_.end()) {
				set_name(durable_names_, path, now->second);
			} else if (const auto was = durable_names_.find(path); was != durable_names_.end()) {
				erase_name(durable_names_, was);
			}
		}
		for (const std::uint64_t id : dropped) { unref(id, false); }
#if FAULTLAB_CHECK_COUNTS
		for (const auto* m : {&names_, &durable_names_}) {
			const auto* other = m == &names_ ? &durable_names_ : &names_;
			for (const auto& [n, id] : *m) {
				const auto it = other->find(n);
				if (detail::parent(n) == directory && (it == other->end() || it->second != id)) { throw std::logic_error("faultfs: " + n + " not durable after its directory sync"); }
			}
		}
#endif
	}

	// The coins a policy fixes.
	static detail::coin_force forced(crash_policy p) {
		if (p.epochs == epoch_pick::coin && p.names == name_pick::coin && (p.size == size_pick::coin || p.size == size_pick::sector) && p.sectors == sector_pick::coin) { return {}; }
		return [p](detail::coin_site site, op kind) -> std::optional<bool> {
			switch (site) {
			case detail::coin_site::epoch:
				if (p.epochs == epoch_pick::coin) { return std::nullopt; }
				return p.epochs == epoch_pick::all;
			case detail::coin_site::name:
				if (p.names == name_pick::coin) { return std::nullopt; }
				if (p.names == name_pick::additions) { return kind != op::remove; }
				return p.names == name_pick::all;
			case detail::coin_site::size:  // true keeps the old size
				if (p.size == size_pick::durable) { return true; }
				if (p.size == size_pick::current) { return false; }
				return std::nullopt;
			case detail::coin_site::sector:  // true takes the new bytes
				if (p.sectors == sector_pick::coin) { return std::nullopt; }
				return p.sectors == sector_pick::fresh;
			}
			return std::nullopt;
		};
	}

	// crash(): a forced coin is drawn and replaced, so a fixed pick keeps the stream; only a size or
	// epoch pick changes how many coins follow (the sector loop runs to the size, the epoch loop
	// stops at its first lost epoch).
	void rebuild(std::uint64_t seed, const detail::coin_force& force, bool sector_size) {
		fold_locked();
		std::mt19937_64 rng(seed);
		auto coin = [&](detail::coin_site site, op kind = op::write) {
			const bool flip = (rng() & 1) != 0;
			if (force) {
				if (auto v = force(site, kind)) { return *v; }
			}
			return flip;
		};
		// An ordering point holds: either everything staged before it survives (and later changes
		// may), or later changes do not. The first `whole` epochs survive whole.
		const std::size_t epochs = marks_.size();
		std::size_t whole = 0;
		while (whole < epochs && coin(detail::coin_site::epoch)) { ++whole; }
		// Data: per inode still reachable from either namespace.
		for (auto& [id, node] : inodes_) {
			if (!node->dirty) {
				if (!node->lost.empty()) {
					node->current = node->durable;  // a lost page reads its durable bytes again
					node->lost.clear();
				}
				continue;
			}
			// The surviving epochs' staged bytes survive whole; the first lost epoch's bytes may
			// survive in part over them, and nothing written after its ordering point does.
			const std::string* base = nullptr;  // the newest bytes a surviving epoch staged
			for (const auto& [e, bytes] : node->staged) {
				if (e < whole) { base = &bytes; }
			}
			const std::string* cut = whole < epochs ? node->as_of(whole) : nullptr;  // as of the first lost epoch
			const std::string now = cut ? *cut : node->flushed();
			if (base && now == *base) {
				node->durable = now;
				node->current = now;
				node->lost.clear();
				node->dirty = false;
				continue;
			}
			const std::string& old = base ? *base : node->durable;
			std::size_t size = 0;
			if (sector_size) {
				// Any sector boundary between the two sizes, or either size: a cut partway through
				// an unsynced append (one draw, where the coin would be).
				const std::size_t lo = std::min(old.size(), now.size()), hi = std::max(old.size(), now.size());
				std::vector<std::size_t> cuts{lo};
				for (std::size_t at = (lo / detail::sector + 1) * detail::sector; at < hi; at += detail::sector) { cuts.push_back(at); }
				if (hi != lo) { cuts.push_back(hi); }
				size = cuts[rng() % cuts.size()];
			} else {
				size = coin(detail::coin_site::size) ? old.size() : now.size();
			}
			std::string out(size, '\0');
			// Each sector holds its old bytes or its new ones; past the durable size, its new ones
			// or zeros (growth persisting without its data).
			for (std::size_t at = 0; at < size; at += detail::sector) {
				const std::size_t n = std::min(detail::sector, size - at);
				const bool take_new = at < now.size() && coin(detail::coin_site::sector);
				const std::string& from = take_new ? now : old;
				if (at < from.size()) { std::memcpy(out.data() + at, from.data() + at, std::min(n, from.size() - at)); }
			}
			node->current = out;
			node->durable = std::move(out);
			node->lost.clear();
			node->dirty = false;
		}
		for (auto& [id, node] : inodes_) { node->unmapped.clear(); }
		// Namespace: the durable map plus a surviving subset of the logged ops, in order. Ops before
		// the last whole epoch's mark survive, ops up to the first lost epoch's mark may, and later
		// ones do not.
		auto names = durable_names_;
		const std::size_t kept = whole ? marks_[whole - 1] : 0, cut_at = whole < epochs ? marks_[whole] : log_.size();
		std::set<std::uint64_t> unlinked;  // files a dropped link would have named
		std::size_t at = 0;
		for (const auto& e : log_) {
			const std::size_t i = at++;
			const bool survives = i < cut_at && (i < kept || coin(detail::coin_site::name, e.kind));
			if (e.kind == op::link) {
				const auto source = names.find(e.a);
				if (!survives || source == names.end() || source->second != e.id || names.count(e.b)) {
					unlinked.insert(e.id);
					continue;
				}
			}
			if (!survives) { continue; }
			// A remove or a replace's overwrite cannot outlive the link that kept its file named.
			if (e.kind == op::remove && unlinked.count(e.id)) { continue; }
			if (e.kind == op::replace) {
				if (auto d = names.find(e.b); d != names.end() && d->second != e.id && unlinked.count(d->second)) { continue; }
			}
			apply(names, e);
		}
		names_ = names;
		durable_names_ = std::move(names);
		log_.clear();
		clear_marks();
		strays_.clear();
		for (auto& [id, node] : inodes_) { node->staged.clear(); }
		recount();
		collect();
		release();
		power_left_.reset();
	}
	// What crash and kill share: handles stale, locks, leases and mappings released.
	void release() {
		for (auto* r : shared_) {
			if (--r->node->mappings == 0) { r->node->mapped = 0; }
		}
		shared_.clear();
		for (auto& [id, node] : inodes_) { node->leases.clear(); }
		++epoch_;
		owners_.clear();
		directory_owners_.clear();
	}

	// Folds every shared mapping's changed sectors into its file.
	void fold_locked() const {
		for (auto* r : shared_) { fold(*r); }
	}
	// With the power off, a store reaches nothing: the cut is the instant the power ran out.
	void fold(detail::shared_region& r) const {
		if (power_left_ && *power_left_ == 0) { return; }
		auto& node = *r.node;
		const std::size_t end = std::min(r.words.size(), node.current.size());  // past the end, a store is lost
		for (std::size_t at = 0; at < end; at += detail::sector) {
			const std::size_t n = std::min(detail::sector, end - at);
			if (std::memcmp(node.current.data() + at, r.words.data() + at, n) == 0) { continue; }
			node.note_write(marks_.size());
			node.unmapped.try_emplace(at / detail::sector, node.current.substr(at, n));
			std::memcpy(node.current.data() + at, r.words.data() + at, n);
			node.touch(at, at + n);
			node.dirty = true;
		}
	}
	// After a write or a dropped sync changed `node`'s bytes: its mappings see them.
	void refresh(const detail::inode& node, std::uint64_t begin, std::uint64_t end) {
		for (auto* r : shared_) {
			if (r->node.get() != &node || begin >= r->words.size()) { continue; }
			const std::uint64_t stop = std::min<std::uint64_t>(end, r->words.size());
			std::memcpy(r->words.data() + begin, node.current.data() + begin, stop - begin);
		}
	}

	void copy_from(const filesystem& o) {
		std::map<const detail::inode*, std::shared_ptr<detail::inode>> copies;
		for (const auto& [id, node] : o.inodes_) {
			auto c = std::make_shared<detail::inode>(*node);
			c->mapped = c->mappings = 0;
			c->leases.clear();
			copies[node.get()] = c;
			inodes_[id] = c;
		}
		names_ = o.names_;
		durable_names_ = o.durable_names_;
		directories_ = o.directories_;
		log_ = o.log_;
		marks_ = o.marks_;
		strays_ = o.strays_;
		rules_ = o.rules_;
		for (const auto& m : o.media_) {
			auto c = m;
			auto it = copies.find(m.node.get());
			c.node = it != copies.end() ? it->second : std::make_shared<detail::inode>(*m.node);
			media_.push_back(std::move(c));
		}
		media_hits_ = o.media_hits_;
		map_files_ = o.map_files_;
		next_id_ = o.next_id_;
		next_handle_ = o.next_handle_;
		capacity_ = o.capacity_;
		short_ = o.short_;
		damage_min_ = o.damage_min_;
		damage_at_ = o.damage_at_;
		damage_left_ = o.damage_left_;
		damaged_ = o.damaged_;
		class_ = o.class_;
		trust_ = o.trust_;
		mutations_ = o.mutations_;
		calls_ = o.calls_;
		syncs_ = o.syncs_;
		power_left_ = o.power_left_;
		std::copy(std::begin(o.ops_), std::end(o.ops_), ops_);
		sync_latency_.store(o.sync_latency_.load());
		recount();
	}
	void take(filesystem&& o) {
		inodes_ = std::move(o.inodes_);
		names_ = std::move(o.names_);
		durable_names_ = std::move(o.durable_names_);
		directories_ = std::move(o.directories_);
		log_ = std::move(o.log_);
		marks_ = std::move(o.marks_);
		strays_ = std::move(o.strays_);
		unreferenced_ = std::move(o.unreferenced_);
		used_ = o.used_;
		rules_ = std::move(o.rules_);
		media_ = std::move(o.media_);
		shared_ = std::move(o.shared_);
		owners_ = std::move(o.owners_);
		directory_owners_ = std::move(o.directory_owners_);
		media_hits_ = o.media_hits_;
		map_files_ = o.map_files_;
		next_id_ = o.next_id_;
		next_handle_ = o.next_handle_;
		epoch_ = o.epoch_;
		capacity_ = o.capacity_;
		short_ = o.short_;
		damage_min_ = o.damage_min_;
		damage_at_ = o.damage_at_;
		damage_left_ = o.damage_left_;
		damaged_ = o.damaged_;
		class_ = std::move(o.class_);
		trust_ = o.trust_;
		mutations_ = o.mutations_;
		calls_ = o.calls_;
		syncs_ = o.syncs_;
		power_left_ = o.power_left_;
		std::copy(std::begin(o.ops_), std::end(o.ops_), ops_);
		sync_latency_.store(o.sync_latency_.load());
	}

	mutable std::mutex mu_;
	std::map<std::uint64_t, std::shared_ptr<detail::inode>> inodes_;
	name_map names_, durable_names_;  // by path: "name" or "dir/name"
	std::set<std::string> directories_;  // every directory but the root, durable at once
	std::vector<logged> log_;
	std::vector<std::size_t> marks_;  // per ordering point: log_ entries before it are staged ahead of those after
	std::set<std::uint64_t> strays_;  // inodes staged after their ordering point settled (another thread's sync_directory between)
	std::set<std::uint64_t> unreferenced_;  // inodes with no refs, freed once no handle holds them
	std::uint64_t used_ = 0;  // every inode's footprint, as accounted
	std::vector<rule> rules_;
	struct media_rule {
		std::shared_ptr<detail::inode> node;
		media kind;
		std::uint64_t begin, end, left;
		bool sticky = left == 0;
	};
	std::vector<media_rule> media_;
	std::vector<detail::shared_region*> shared_;  // live, attached shared mappings
	std::set<std::string> owners_, directory_owners_;  // directories whose owner.lock or directory lock is held
	std::uint64_t media_hits_ = 0;
	bool map_files_ = false;
	std::uint64_t next_id_ = 1, next_handle_ = 1, epoch_ = 0, capacity_ = ~std::uint64_t(0);
	std::size_t short_ = 0;
	std::size_t damage_min_ = 0, damage_at_ = 0;
	int damage_left_ = 0, damaged_ = 0;
	journal::FilesystemClass class_{true, "faultfs"};
	journal::DirectoryTrust trust_{true, false, false, false};
	std::uint64_t mutations_ = 0, calls_ = 0, syncs_ = 0;
	std::uint64_t ops_[std::size_t(op::lease) + 1] = {};  // per kind
	std::atomic<std::int64_t> sync_latency_{0};  // microseconds
	std::optional<std::uint64_t> power_left_;
};

namespace detail {

// Test access to crash() with chosen coins forced: a fixed pick must equal this with its field's
// coins forced, and the other fields' coins drawn from the same stream.
struct crash_access {
	static void crash(filesystem& fs, std::uint64_t seed, const coin_force& force, bool sector_size = false) {
		std::lock_guard lock(fs.mu_);
		fs.rebuild(seed, force, sector_size);
	}
};

// The recorder's view of a model: every directory and every file's current bytes, by path.
struct model_access {
	static std::set<std::string> directories(const filesystem& fs) {
		std::lock_guard lock(fs.mu_);
		fs.fold_locked();
		return fs.directories_;
	}
	static std::map<std::string, std::string> files(const filesystem& fs) {
		std::lock_guard lock(fs.mu_);
		fs.fold_locked();
		std::map<std::string, std::string> out;
		for (const auto& [path, id] : fs.names_) { out[path] = fs.inodes_.at(id)->current; }
		return out;
	}
};

inline journal::EntryInspection inspection(const inode& node, std::uint64_t links) {
	journal::EntryInspection out;
	out.footprint = {journal::EntryKind::Regular, node.current.size(), std::nullopt};
	out.device = 1;
	out.inode = node.id;
	out.owner = ::geteuid();
	out.links = links;
	out.permissions = 0600;
	return out;
}

}  // namespace detail

}  // namespace kronuz::faultlab
