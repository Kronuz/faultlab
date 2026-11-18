// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs: an in-memory kronuz::journal::IO that models what storage keeps across power loss,
// for crash testing. One filesystem is one device; an io is one directory on it. Each file has
// current bytes and durable bytes; each directory has a current namespace and a durable one.
// File::sync makes a file's bytes durable, sync_directory makes that directory's namespace
// durable. crash() rebuilds the state a power loss could leave:
//
//   - each 512-byte sector written since the file's last sync keeps its old bytes, takes its
//     new ones, or (beyond the durable size) reads as zeros; the size is the durable one or the
//     current one (or, with size_pick::sector, any sector boundary between them);
//   - each create, link, rename and unlink since its directory's last sync survives or not,
//     replayed in order, a rename atomically; an op whose source no longer exists drops, and an
//     op that drops a name of a file whose earlier link dropped drops too.
//
// IO::order_files_and_directory stages the files' bytes and the namespace changes so far ahead
// of every later change, without making them durable: after it, a create, link, rename or unlink
// may survive a crash only if every staged byte and namespace change survived too; the next
// sync_directory makes the staged bytes durable with the namespace, and so does the next
// File::sync (the barrier's second phase: a sync that returns has flushed what the barrier
// ordered ahead of it). An ordering point is device-wide. A write to a staged file after the
// ordering point is not ordered: it stays unsynced over the staged bytes until that file's own
// sync, and never survives without them. Ordering points chain: a crash keeps the epochs before
// some point whole, that point's epoch in part, and nothing after it.
//
// Faults: a rule fails the n-th operation of a kind, before or after its effect, with EIO or
// ENOSPC; a capacity fails writes and preallocations that grow the total past it with ENOSPC;
// short writes return part of the request. A failed File::sync drops the file's unsynced bytes;
// a later sync succeeds. Handles opened before a crash or a kill() fail with EIO.
//
// Media faults model unreliable storage on reads: a read overlapping a file's byte range fails
// with EIO (a latent sector error) or returns one bit flipped in that range, the next `times`
// reads (transient) or every read (sticky, times 0). The stored bytes never change.
//
// Shared mappings (File::map_shared) are buffers folded into the file at every call into the
// filesystem, before that call takes effect, and at crash, kill and snapshot: each changed
// sector is written as a write there would be, bypassing faults, the power counter and
// mutations(). Do not store into a mapping while another thread calls in.
#pragma once

#include "completion/io.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace kronuz::faultlab {

enum class op { write, sync, sync_directory, create, replace, remove, truncate, link, preallocate, lease };

// Fails the `nth` (1-based) operation of `kind` counted from when the rule is added, before or
// after its effect takes place. A failed sync with `keep` leaves the unsynced pages readable and
// clean but not durable, as Linux does after a writeback error: no later sync writes them unless
// a write or truncate dirties them again.
struct fault {
	op kind;
	std::uint64_t nth = 1;
	int error = EIO;
	bool after = false;
	bool keep = false;
};

enum class media { read_error, bit_flip };

// What crash() fixes instead of flipping a coin. Every fixed pick is a state the IO contract
// allows; the default, `coin` everywhere, consumes the seed's stream as it always has.
enum class epoch_pick { coin, none, all };               // ordering points kept whole
enum class name_pick { coin, durable, all, additions };  // namespace ops the epoch cut admits
enum class size_pick { coin, durable, current, sector }; // each dirty file's size
enum class sector_pick { coin, old, fresh };             // each 512-byte sector's bytes
struct crash_policy {
	epoch_pick epochs = epoch_pick::coin;
	name_pick names = name_pick::coin;
	size_pick size = size_pick::coin;
	sector_pick sectors = sector_pick::coin;
};

class filesystem;

namespace detail {

inline constexpr std::size_t sector = 512;
inline constexpr std::size_t page = 4096;

struct inode {
	std::uint64_t id = 0;
	std::string current;  // what reads return
	std::string durable;  // what survives a crash for certain
	bool dirty = false;
	std::vector<std::pair<std::size_t, std::string>> staged;  // per ordering point (its epoch), the bytes it ordered ahead of later changes
	std::vector<std::pair<std::size_t, std::string>> at_mark;  // per ordering point a later write followed, the bytes before that write
	std::uint64_t mapped = 0;  // the longest live mapping's length (a real one would fault past a cut)
	std::uint64_t mappings = 0;
	std::uint64_t reserved = 0;  // the extent preallocate reserved
	std::set<std::uint64_t> lost;  // pages a failed sync left clean but not durable (fault::keep)
	std::map<std::uint64_t, journal::LeaseMode> leases;  // by holding handle
	// Per sector a shared mapping changed: its bytes without the mapped stores. File::sync leaves
	// these sectors so (io.h promises mapped stores no durability on macOS); an ordering point or a
	// write_at over the whole sector ends it.
	std::map<std::uint64_t, std::string> unmapped;

	// What a flush makes durable: the current bytes, the lost pages keeping their durable ones
	// (zeros past the durable size).
	std::string flushed() const {
		std::string out = current;
		for (std::uint64_t p : lost) {
			for (std::uint64_t at = p * page; at < std::min<std::uint64_t>((p + 1) * page, out.size()); ++at) { out[at] = at < durable.size() ? durable[at] : '\0'; }
		}
		return out;
	}
	// What File::sync makes durable: the flushed bytes, each mapped sector without its stores.
	std::string synced() const {
		std::string out = flushed();
		for (const auto& [s, bytes] : unmapped) {
			const std::uint64_t at = s * sector;
			if (at < out.size()) { std::memcpy(out.data() + at, bytes.data(), std::min<std::uint64_t>(bytes.size(), out.size() - at)); }
		}
		return out;
	}
	// A write_at over [begin, end) reaches the unmapped bytes of the sectors it overlaps; a write
	// over a whole sector ends that sector's entry.
	void overwrite_unmapped(std::uint64_t begin, std::uint64_t end) {
		for (auto it = unmapped.lower_bound(begin / sector); it != unmapped.end() && it->first * sector < end;) {
			const std::uint64_t at = it->first * sector;
			if (begin <= at && at + sector <= end) {
				it = unmapped.erase(it);
				continue;
			}
			auto& bytes = it->second;
			const std::uint64_t from = std::min<std::uint64_t>(std::max(begin, at), at + bytes.size());
			const std::uint64_t to = std::min<std::uint64_t>({end, at + sector, current.size()});
			if (to > at + bytes.size()) { bytes.resize(to - at); }
			if (to > from) { std::memcpy(bytes.data() + (from - at), current.data() + from, to - from); }
			++it;
		}
	}
	// A truncate to `size` cuts the unmapped bytes with the file.
	void truncate_unmapped(std::uint64_t size) {
		unmapped.erase(unmapped.lower_bound((size + sector - 1) / sector), unmapped.end());
		if (size % sector) {
			if (auto it = unmapped.find(size / sector); it != unmapped.end()) { it->second.resize(std::min<std::uint64_t>(it->second.size(), size % sector)); }
		}
	}
	// A failed sync with `keep`: every page that differs from the durable bytes is lost.
	void lose_unsynced() {
		for (std::uint64_t at = 0; at < current.size(); at += page) {
			const std::uint64_t n = std::min<std::uint64_t>(page, current.size() - at);
			if (at + n > durable.size() || std::memcmp(current.data() + at, durable.data() + at, n) != 0) { lost.insert(at / page); }
		}
	}
	// The bytes as of ordering point `epoch`: a later write never survives without that epoch.
	const std::string* as_of(std::size_t epoch) const {
		const std::string* best = nullptr;
		std::size_t at = ~std::size_t(0);
		for (const auto* v : {&staged, &at_mark}) {
			for (const auto& [e, bytes] : *v) {
				if (e >= epoch && e < at) {
					at = e;
					best = &bytes;
				}
			}
		}
		return best;
	}
	// Before a write or truncate: the bytes as of the latest ordering point, once per point.
	void note_write(std::size_t marks) {
		if (!marks) { return; }
		const std::size_t e = marks - 1;
		for (const auto& [k, b] : staged) { if (k == e) { return; } }
		for (const auto& [k, b] : at_mark) { if (k == e) { return; } }
		at_mark.emplace_back(e, flushed());
	}
	// A failed sync: the bytes no ordering point staged are dropped; what one ordered stays ordered.
	void drop_unstaged() {
		current = staged.empty() ? durable : staged.back().second;
		lost.clear();
		unmapped.clear();
		dirty = current != durable;
	}
	// What the ordering points staged becomes durable; a write after them stays unsynced.
	void settle() {
		durable = std::move(staged.back().second);
		staged.clear();
		dirty = flushed() != durable;
	}
	// A write or truncate dirties the pages from `begin` up to `end`.
	void touch(std::uint64_t begin, std::uint64_t end) {
		if (lost.empty() || end <= begin) { return; }
		lost.erase(lost.lower_bound(begin / page), end == ~std::uint64_t(0) ? lost.end() : lost.lower_bound((end - 1) / page + 1));
	}
};

// A shared mapping's words, folded into its inode at every call into the filesystem.
struct shared_region {
	std::shared_ptr<inode> node;
	std::vector<char> words;
};

[[noreturn]] inline void fail(int error, const char* what) { throw std::system_error(error, std::generic_category(), what); }

inline void check_name(std::string_view name) {
	if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
		throw std::invalid_argument("faultfs: name must be a single component");
	}
}

inline std::string join(std::string_view directory, std::string_view name) {
	return directory.empty() ? std::string(name) : std::string(directory) + "/" + std::string(name);
}

inline std::string parent(std::string_view path) {
	const auto slash = path.rfind('/');
	return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

inline std::string base(std::string_view path) {
	const auto slash = path.rfind('/');
	return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// Where crash() flips a coin, for forcing them (crash_access).
enum class coin_site { epoch, name, size, sector };
// A forced coin's value, or nullopt to keep the flip; `kind` is the logged op at a name coin.
using coin_force = std::function<std::optional<bool>(coin_site, op kind)>;
struct crash_access;
struct model_access;

}  // namespace detail

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
	// Every call into an IO or File so far, reads, opens, maps and queries included (not a
	// mapping's or lock's release): a code path that makes no device call leaves it unchanged.
	std::uint64_t calls() const {
		std::lock_guard lock(mu_);
		return calls_;
	}
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
		std::uint64_t n = 0;
		for (const auto& [_, node] : inodes_) { n += std::max({node->current.size(), node->durable.size(), std::size_t(node->reserved)}); }
		return n;
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
		std::uint64_t n = 0;
		for (const auto& [_, i] : names_) { n += i == id; }
		return n;
	}
	// Drops inodes no name, durable name or logged op references and no handle holds; one pass
	// over each, so a call is linear in names + log + inodes.
	void collect() {
		std::unordered_set<std::uint64_t> referenced;
		referenced.reserve(names_.size() + durable_names_.size() + log_.size());
		for (const auto& [_, id] : names_) { referenced.insert(id); }
		for (const auto& [_, id] : durable_names_) { referenced.insert(id); }
		for (const auto& e : log_) { referenced.insert(e.id); }
		for (auto it = inodes_.begin(); it != inodes_.end();) {
			if (it->second.use_count() == 1 && !referenced.count(it->first)) {
				it = inodes_.erase(it);
			} else {
				++it;
			}
		}
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
		case op::replace:
			if (auto it = names.find(e.a); it != names.end() && it->second == e.id) {
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
	}
	// sync_directory: what the ordering points staged, then `directory`'s namespace, durable.
	void settle_directory(const std::string& directory) {
		settle_staged();
		std::erase_if(durable_names_, [&](const auto& entry) { return detail::parent(entry.first) == directory; });
		for (const auto& [n, id] : names_) {
			if (detail::parent(n) == directory) { durable_names_[n] = id; }
		}
		std::erase_if(log_, [&](const logged& e) { return detail::parent(e.a) == directory; });
		for (auto& [id, node] : inodes_) {
			if (!node->staged.empty()) { node->settle(); }
		}
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
		for (auto& [id, node] : inodes_) { node->staged.clear(); }
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
	}
	void take(filesystem&& o) {
		inodes_ = std::move(o.inodes_);
		names_ = std::move(o.names_);
		durable_names_ = std::move(o.durable_names_);
		directories_ = std::move(o.directories_);
		log_ = std::move(o.log_);
		marks_ = std::move(o.marks_);
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
	}

	mutable std::mutex mu_;
	std::map<std::uint64_t, std::shared_ptr<detail::inode>> inodes_;
	std::map<std::string, std::uint64_t> names_, durable_names_;  // by path: "name" or "dir/name"
	std::set<std::string> directories_;  // every directory but the root, durable at once
	std::vector<logged> log_;
	std::vector<std::size_t> marks_;  // per ordering point: log_ entries before it are staged ahead of those after
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

class file final : public journal::File {
public:
	// A read-only file reads, syncs and closes; write_at, truncate, preallocate and map_shared fail.
	file(filesystem& fs, std::shared_ptr<detail::inode> node, bool read_only = false) : fs_(&fs), node_(std::move(node)), epoch_(fs.epoch_), id_(fs.next_handle_++), read_only_(read_only) {}
	~file() override {
		std::lock_guard lock(fs_->mu_);
		node_->leases.erase(id_);
	}

	std::uint64_t size() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return node_->current.size();
	}
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const std::string& d = node_->current;
		if (offset >= d.size()) { return 0; }
		const std::size_t n = std::min<std::uint64_t>(bytes.size(), d.size() - offset);
		std::memcpy(bytes.data(), d.data() + offset, n);
		for (auto& m : fs_->media_) {
			if (m.node != node_ || m.end <= offset || m.begin >= offset + n || (!m.sticky && m.left == 0)) { continue; }
			if (!m.sticky) { --m.left; }
			++fs_->media_hits_;
			if (m.kind == media::read_error) { detail::fail(EIO, "faultfs read"); }
			const std::uint64_t at = std::max(m.begin, offset);
			bytes[at - offset] = char(bytes[at - offset] ^ (1 << (at % 8)));
		}
		return n;
	}
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs write"); }
		const fault* f = fs_->check(op::write);
		if (f && !f->after) { detail::fail(f->error, "faultfs write"); }
		std::size_t n = bytes.size();
		if (fs_->short_ && n > fs_->short_) { n = fs_->short_; }
		const std::uint64_t end = offset + n;
		const std::uint64_t held = std::max({node_->current.size(), node_->durable.size(), std::size_t(node_->reserved)});
		if (end > held && fs_->used_locked() + (end - held) > fs_->capacity_) { detail::fail(ENOSPC, "faultfs write"); }
		node_->note_write(fs_->marks_.size());
		if (end > node_->current.size()) { node_->current.resize(end, '\0'); }
		std::memcpy(node_->current.data() + offset, bytes.data(), n);
		node_->touch(offset, end);
		if (fs_->damage_left_ > 0 && n >= fs_->damage_min_ && fs_->damage_at_ < n) {
			node_->current[offset + fs_->damage_at_] ^= 0x10;
			--fs_->damage_left_;
			++fs_->damaged_;
		}
		node_->overwrite_unmapped(offset, end);
		node_->dirty = true;
		fs_->refresh(*node_, offset, end);
		if (f) { detail::fail(f->error, "faultfs write"); }
		return n;
	}
	void truncate(std::uint64_t size) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs truncate"); }
		const fault* f = fs_->check(op::truncate);
		if (f && !f->after) { detail::fail(f->error, "faultfs truncate"); }
		if (node_->mappings && size < node_->mapped) {
			std::fprintf(stderr, "faultfs: a file truncated below a live mapping\n");
			std::abort();
		}
		node_->note_write(fs_->marks_.size());
		node_->current.resize(size, '\0');
		node_->truncate_unmapped(size);
		node_->touch(size, ~std::uint64_t(0));
		node_->reserved = std::min(node_->reserved, size);
		node_->dirty = true;
		if (f) { detail::fail(f->error, "faultfs truncate"); }
	}
	// Grows the file to at least offset + length, zeros, and reserves that extent against the
	// capacity: a mutation, failed by a fault rule, the power or a full device before any effect.
	void preallocate(std::uint64_t offset, std::uint64_t length) override {
		if (length > ~std::uint64_t{0} - offset) { throw std::length_error("preallocation exceeds the file range"); }
		std::lock_guard lock(fs_->mu_);
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs preallocate"); }
		const fault* f = fs_->check(op::preallocate);
		if (f && !f->after) { detail::fail(f->error, "faultfs preallocate"); }
		const std::uint64_t end = offset + length;
		const std::uint64_t held = std::max({node_->current.size(), node_->durable.size(), std::size_t(node_->reserved)});
		if (end > held && fs_->used_locked() + (end - held) > fs_->capacity_) { detail::fail(ENOSPC, "faultfs preallocate"); }
		if (end > node_->current.size()) {
			const std::uint64_t was = node_->current.size();
			node_->note_write(fs_->marks_.size());
			node_->current.resize(end, '\0');
			node_->touch(was, end);
			node_->dirty = true;
		}
		node_->reserved = std::max(node_->reserved, end);
		if (f) { detail::fail(f->error, "faultfs preallocate"); }
	}
	// A copy of the file's bytes, registered on the inode while it lives.
	std::shared_ptr<const journal::Mapping> map() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (!fs_->map_files_) { return nullptr; }
		return std::make_shared<mapping>(*fs_, node_);
	}
	// A buffer over [0, length), folded into the file at every call into the filesystem.
	std::shared_ptr<journal::SharedMapping> map_shared(std::uint64_t length) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (read_only_) { detail::fail(EACCES, "faultfs map_shared"); }
		if (length > node_->current.size()) { detail::fail(EINVAL, "faultfs map_shared past the end"); }
		return std::make_shared<shared_mapping>(*fs_, node_, length);
	}
	void sync() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (const fault* f = fs_->check(op::sync)) {
			if (f == &filesystem::power_off_) { detail::fail(f->error, "faultfs sync"); }
			// A failed fsync loses the unsynced bytes; reads see the durable ones, or with `keep`
			// still the cached ones. One that failed after its effect flushed them, and what an
			// ordering point staged before them.
			if (f->after) {
				node_->durable = node_->synced();
				fs_->settle_staged();
			}
			if (f->keep && !f->after) {
				node_->lose_unsynced();
				node_->dirty = false;
			} else {
				node_->drop_unstaged();
				fs_->refresh(*node_, 0, node_->current.size());
			}
			detail::fail(f->error, "faultfs sync");
		}
		node_->durable = node_->synced();
		node_->dirty = !node_->unmapped.empty();
		node_->staged.clear();
		node_->at_mark.clear();
		fs_->settle_staged();
	}
	journal::EntryInspection inspect() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return detail::inspection(*node_, fs_->links_of(node_->id));
	}
	// A per-inode lease: shared ones coexist, an exclusive one excludes all. Released when this
	// handle closes or is destroyed, and by kill() and crash().
	bool try_lease(journal::LeaseMode mode) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (node_->leases.count(id_)) { throw std::logic_error("faultfs: a lease on a handle that holds one"); }
		const fault* f = fs_->rule_for(op::lease);
		if (f && !f->after) { detail::fail(f->error, "faultfs lease"); }
		bool free = true;
		for (const auto& [_, held] : node_->leases) { free = free && mode == journal::LeaseMode::Shared && held == journal::LeaseMode::Shared; }
		if (free) { node_->leases[id_] = mode; }
		if (f) { detail::fail(f->error, "faultfs lease"); }
		return free;
	}
	void close() override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		node_->leases.erase(id_);
		closed_ = true;
	}

	// Stages the bytes ahead of later changes for ordering point `epoch` (see
	// io::order_files_and_directory); durable at the next sync_directory.
	void stage(std::size_t epoch) {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (const fault* f = fs_->check(op::sync)) {
			if (f != &filesystem::power_off_) {
				node_->drop_unstaged();
				fs_->refresh(*node_, 0, node_->current.size());
			}
			detail::fail(f->error, "faultfs stage");
		}
		if (node_->dirty) { node_->staged.emplace_back(epoch, node_->flushed()); }
		node_->unmapped.clear();  // the point orders the mapped stores as writes made here
	}

private:
	class mapping final : public journal::Mapping {
	public:
		mapping(filesystem& fs, std::shared_ptr<detail::inode> node) : fs_(&fs), node_(std::move(node)), copy_(node_->current) {
			bytes_ = copy_;
			++node_->mappings;
			node_->mapped = std::max<std::uint64_t>(node_->mapped, copy_.size());
		}
		~mapping() override {
			std::lock_guard lock(fs_->mu_);
			if (--node_->mappings == 0) { node_->mapped = 0; }
		}

	private:
		filesystem* fs_;
		std::shared_ptr<detail::inode> node_;
		std::string copy_;
	};
	class shared_mapping final : public journal::SharedMapping {
	public:
		shared_mapping(filesystem& fs, std::shared_ptr<detail::inode> node, std::uint64_t length) : fs_(&fs) {
			region_.node = std::move(node);
			region_.words.assign(region_.node->current.begin(), region_.node->current.begin() + std::ptrdiff_t(length));
			view(std::span<char>(region_.words));
			++region_.node->mappings;
			region_.node->mapped = std::max<std::uint64_t>(region_.node->mapped, length);
			fs_->shared_.push_back(&region_);
		}
		// Unmapping keeps the stores: they fold, unless a crash or kill detached the mapping.
		~shared_mapping() override {
			std::lock_guard lock(fs_->mu_);
			auto it = std::find(fs_->shared_.begin(), fs_->shared_.end(), &region_);
			if (it == fs_->shared_.end()) { return; }
			fs_->fold(region_);
			fs_->shared_.erase(it);
			if (--region_.node->mappings == 0) { region_.node->mapped = 0; }
		}

	private:
		filesystem* fs_;
		detail::shared_region region_;
	};
	// Every call: counted, stale and closed handles refused, shared mappings folded first.
	void enter() {
		++fs_->calls_;
		if (epoch_ != fs_->epoch_) { detail::fail(EIO, "faultfs handle from before a crash"); }
		if (closed_) { detail::fail(EBADF, "faultfs closed handle"); }
		fs_->fold_locked();
	}
	filesystem* fs_;
	std::shared_ptr<detail::inode> node_;
	std::uint64_t epoch_;
	std::uint64_t id_;
	bool read_only_;
	bool closed_ = false;
};

class io final : public journal::IO {
public:
	// The IO of `directory` ("" the root, else one make_directory made) on `fs`.
	explicit io(filesystem& fs, std::string directory = {}) : fs_(&fs), directory_(std::move(directory)) {
		std::lock_guard lock(fs_->mu_);
		if (!fs_->directory_exists(directory_)) { detail::fail(ENOENT, "faultfs io directory"); }
	}

	using journal::IO::acquire_owner;
	std::unique_ptr<journal::OwnerLock> acquire_owner(bool create) override { return lock_owner(journal::OwnerLockOptions::legacy_file(create)); }
	std::unique_ptr<journal::OwnerLock> acquire_directory_owner() override { return lock_owner(journal::OwnerLockOptions::legacy_directory()); }
	journal::FilesystemClass directory_filesystem() const override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		return fs_->class_;
	}
	journal::DirectoryTrust directory_trust() const override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		return fs_->trust_;
	}
	std::unique_ptr<journal::File> open_existing(std::string_view name) override { return open(name, false); }
	std::unique_ptr<journal::File> open_existing_read_only(std::string_view name) override { return open(name, true); }
	std::unique_ptr<journal::File> open_reclaim_candidate_read_only(std::string_view name) override { return open(name, true); }
	void open_existing_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable file open unsupported"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable candidate open unsupported"); }
	std::unique_ptr<journal::File> create_exclusive(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::create);
		if (f && !f->after) { detail::fail(f->error, "faultfs create"); }
		if (exists(name)) { detail::fail(EEXIST, "faultfs create"); }
		auto node = make_locked(name);
		if (f) { detail::fail(f->error, "faultfs create"); }
		return std::make_unique<file>(*fs_, std::move(node));
	}
	void replace(std::string_view source, std::string_view destination) override {
		detail::check_name(source);
		detail::check_name(destination);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::replace);
		if (f && !f->after) { detail::fail(f->error, "faultfs replace"); }
		if (is_directory(source) || is_directory(destination)) { detail::fail(EISDIR, "faultfs replace"); }
		auto it = fs_->names_.find(path(source));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs replace"); }
		const std::uint64_t id = it->second;
		fs_->names_.erase(it);
		fs_->names_[path(destination)] = id;
		fs_->log_.push_back({op::replace, path(source), path(destination), id});
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs replace"); }
	}
	// A second name for source's file; never replaces (EEXIST).
	void link(std::string_view source, std::string_view destination) override {
		detail::check_name(source);
		detail::check_name(destination);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::link);
		if (f && !f->after) { detail::fail(f->error, "faultfs link"); }
		if (is_directory(source)) { detail::fail(EPERM, "faultfs link"); }
		auto it = fs_->names_.find(path(source));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs link"); }
		if (exists(destination)) { detail::fail(EEXIST, "faultfs link"); }
		const std::uint64_t id = it->second;
		fs_->names_[path(destination)] = id;
		fs_->log_.push_back({op::link, path(source), path(destination), id});
		if (f) { detail::fail(f->error, "faultfs link"); }
	}
	// Missing names are an idempotent success; a directory fails with EPERM, as unlink does.
	void remove(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::remove);
		if (f && !f->after) { detail::fail(f->error, "faultfs remove"); }
		if (is_directory(name)) { detail::fail(EPERM, "faultfs remove of a directory"); }
		if (auto it = fs_->names_.find(path(name)); it != fs_->names_.end()) {
			fs_->log_.push_back({op::remove, path(name), {}, it->second});
			fs_->names_.erase(it);
			fs_->collect();
		}
		if (f) { detail::fail(f->error, "faultfs remove"); }
	}
	std::unique_ptr<journal::DirectoryCursor> scan_directory() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return std::make_unique<cursor>(fs_->entries(directory_));
	}
	std::optional<journal::EntryFootprint> entry_footprint(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		if (is_directory(name)) { return journal::EntryFootprint{journal::EntryKind::Directory, 0, std::nullopt}; }
		auto it = fs_->names_.find(path(name));
		if (it == fs_->names_.end()) { return std::nullopt; }
		return journal::EntryFootprint{journal::EntryKind::Regular, fs_->inodes_.at(it->second)->current.size(), std::nullopt};
	}
	std::optional<journal::EntryInspection> inspect_entry(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		if (is_directory(name)) {
			journal::EntryInspection out;
			out.footprint = {journal::EntryKind::Directory, 0, std::nullopt};
			out.device = 1;
			out.inode = (std::uint64_t(1) << 63) | std::uint64_t(std::distance(fs_->directories_.begin(), fs_->directories_.find(path(name))));
			out.owner = ::geteuid();
			out.links = 1;
			out.permissions = 0700;
			return out;
		}
		auto it = fs_->names_.find(path(name));
		if (it == fs_->names_.end()) { return std::nullopt; }
		return detail::inspection(*fs_->inodes_.at(it->second), fs_->links_of(it->second));
	}
	// The files' bytes and the namespace so far, staged ahead of whatever follows.
	void order_files_and_directory(std::span<journal::File* const> files) override {
		std::size_t epoch = 0;
		{
			std::lock_guard lock(fs_->mu_);
			fs_->fold_locked();
			epoch = fs_->marks_.size();
			fs_->marks_.push_back(fs_->log_.size());
		}
		bool staged = false;
		for (auto* f : files) {
			if (auto* own = dynamic_cast<file*>(f)) {
				own->stage(epoch);
				staged = true;
			} else {
				f->sync();
			}
		}
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		const fault* f = fs_->check(op::sync_directory);
		if (f && !f->after) {
			if (!staged) { fs_->marks_.pop_back(); }
			detail::fail(f->error, "faultfs order_files_and_directory");
		}
		if (f) { detail::fail(f->error, "faultfs order_files_and_directory"); }
	}
	// This directory's namespace changes durable, with what the ordering points staged.
	void sync_directory() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::sync_directory);
		if (f && !f->after) { detail::fail(f->error, "faultfs sync_directory"); }
		fs_->settle_directory(directory_);
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs sync_directory"); }
	}

protected:
	// owner.lock (file), the directory (directory), or both; busy is EWOULDBLOCK in every mode.
	// Creating owner.lock is logged as a create, but is no mutation and meets no fault rule.
	std::unique_ptr<journal::OwnerLock> lock_owner(const journal::OwnerLockOptions& options) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const bool want_file = options.mode != journal::OwnerLockMode::directory;
		const bool want_directory = options.mode != journal::OwnerLockMode::file;
		if ((want_file && fs_->owners_.count(directory_)) || (want_directory && fs_->directory_owners_.count(directory_))) {
			detail::fail(EWOULDBLOCK, "faultfs owner lock held");
		}
		if (want_file) {
			const bool create = options.owner_file == journal::OwnerFile::create_exclusive;
			if (create == exists("owner.lock")) { detail::fail(create ? EEXIST : ENOENT, "faultfs owner lock"); }
			if (create) { make_locked("owner.lock"); }
			fs_->owners_.insert(directory_);
		}
		if (want_directory) { fs_->directory_owners_.insert(directory_); }
		return std::make_unique<owner>(*fs_, directory_, want_file, want_directory);
	}

private:
	std::unique_ptr<journal::File> open(std::string_view name, bool read_only) {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		if (is_directory(name)) { detail::fail(EISDIR, "faultfs open"); }
		auto it = fs_->names_.find(path(name));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs open"); }
		return std::make_unique<file>(*fs_, fs_->inodes_.at(it->second), read_only);
	}
	struct owner final : journal::OwnerLock {
		owner(filesystem& fs, std::string directory, bool file, bool dir) : fs_(&fs), directory_(std::move(directory)), file_(file), dir_(dir), epoch_(fs.epoch_) {}
		~owner() override {
			std::lock_guard lock(fs_->mu_);
			if (epoch_ != fs_->epoch_) { return; }
			if (file_) { fs_->owners_.erase(directory_); }
			if (dir_) { fs_->directory_owners_.erase(directory_); }
		}
		filesystem* fs_;
		std::string directory_;
		bool file_, dir_;
		std::uint64_t epoch_;
	};
	struct cursor final : journal::DirectoryCursor {
		explicit cursor(std::vector<std::string> names) : names_(std::move(names)) {}
		std::optional<std::string> next() override {
			if (at_ == names_.size()) { return std::nullopt; }
			return names_[at_++];
		}
		std::vector<std::string> names_;
		std::size_t at_ = 0;
	};
	void enter() {
		++fs_->calls_;
		fs_->fold_locked();
	}
	std::string path(std::string_view name) const { return detail::join(directory_, name); }
	bool is_directory(std::string_view name) const { return fs_->directories_.count(path(name)) != 0; }
	bool exists(std::string_view name) const { return fs_->names_.count(path(name)) != 0 || is_directory(name); }
	std::shared_ptr<detail::inode> make_locked(std::string_view name) {
		const std::uint64_t id = fs_->next_id_++;
		auto node = std::make_shared<detail::inode>();
		node->id = id;
		fs_->inodes_[id] = node;
		fs_->names_[path(name)] = id;
		fs_->log_.push_back({op::create, path(name), {}, id});
		return node;
	}
	filesystem* fs_;
	std::string directory_;
};

}  // namespace kronuz::faultlab
