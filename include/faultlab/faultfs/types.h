// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs's options (operations, fault rules, media faults, crash policies) and the state the
// model keeps per file (detail::inode), with the path helpers the other pieces share.
#pragma once

#include "completion/io.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
	std::uint64_t links = 0;  // the current names naming it
	std::uint64_t refs = 0;  // the current names, durable names and logged ops naming it
	std::uint64_t counted = 0;  // its share of filesystem::used(), as last accounted

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
	// The bytes it holds against the capacity.
	std::uint64_t footprint() const { return std::max({current.size(), durable.size(), std::size_t(reserved)}); }
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

}  // namespace kronuz::faultlab
