// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// size_pick::sector: every state journal's MemoryIO (memory_io.h, its power_loss) and storage's
// CrashIO (test_crash.cc's reconstruct: a prefix of the writes, the next one torn to its first
// 512 bytes) leave after random unsynced appends to a synced file is a state faultfs reaches with
// size_pick::sector. CrashIO's appends are whole blocks, as storage writes them.
#include "faultlab/faultfs.h"

#include "journal/memory_io.h"

#include "check.h"

#include <random>
#include <set>

namespace ff = kronuz::faultlab;
namespace mem = kronuz::journal::testing;

namespace {

struct sequence {
	std::string durable;
	std::vector<std::string> appends;
};

std::string random_bytes(std::mt19937_64& rng, std::size_t n) {
	std::string out(n, '\0');
	for (auto& c : out) { c = char('a' + rng() % 26); }
	return out;
}

// The states faultfs reaches for `s` over `seeds` seeds of `policy`.
std::set<std::string> faultfs_states(const sequence& s, ff::crash_policy policy, std::uint64_t seeds = 2048) {
	ff::filesystem base;
	{
		ff::io io(base);
		auto f = io.create_exclusive("f");
		f->write_at(0, s.durable);
		f->sync();
		io.sync_directory();
		std::uint64_t at = s.durable.size();
		for (const auto& a : s.appends) {
			f->write_at(at, a);
			at += a.size();
		}
	}
	std::set<std::string> out;
	for (std::uint64_t seed = 0; seed < seeds; ++seed) {
		auto fs = base.snapshot();
		fs.crash(seed, policy);
		out.insert(std::string(fs.view("f")));
	}
	return out;
}

std::set<std::string> memory_io_states(const sequence& s) {
	std::set<std::string> out;
	for (std::uint64_t seed = 0; seed < 256; ++seed) {
		mem::Disk disk;
		mem::MemoryIO io(disk);
		auto f = io.create_exclusive("f");
		f->write_at(0, s.durable);
		f->sync();
		io.sync_directory();
		std::uint64_t at = s.durable.size();
		for (const auto& a : s.appends) {
			f->write_at(at, a);
			at += a.size();
		}
		disk.random.seed(seed);
		disk.power_loss();
		out.insert(disk.content(disk.inodes.at(disk.names.at("f"))));
	}
	return out;
}

// CrashIO's states from the synced point on: every prefix of the appends, and each prefix with the
// next append torn to its first 512 bytes.
std::set<std::string> crash_io_states(const sequence& s) {
	std::set<std::string> out;
	std::string image = s.durable;
	for (std::size_t k = 0; k <= s.appends.size(); ++k) {
		out.insert(image);
		if (k == s.appends.size()) { break; }
		out.insert(image + s.appends[k].substr(0, 512));
		image += s.appends[k];
	}
	return out;
}

// Appends of 1 to 4 dirty sectors over a synced file, so 2048 seeds reach every faultfs state.
sequence random_sequence(std::mt19937_64& rng, bool blocks) {
	sequence s;
	const std::size_t durable = blocks ? 512 * (rng() % 3) : rng() % 1025;
	s.durable = random_bytes(rng, durable);
	std::size_t room = 2048 - durable;
	const int count = 1 + int(rng() % 4);
	for (int i = 0; i < count && room > 0; ++i) {
		std::size_t n = blocks ? 512 * (1 + rng() % 2) : 1 + rng() % 900;
		n = std::min(n, room);
		s.appends.push_back(random_bytes(rng, n));
		room -= n;
	}
	return s;
}

bool covers(const std::set<std::string>& all, const std::set<std::string>& some) {
	for (const auto& state : some) {
		if (!all.count(state)) { return false; }
	}
	return true;
}

}  // namespace

int main() {
	const ff::crash_policy sector{.size = ff::size_pick::sector};
	faultlab_test::run("every MemoryIO state is a size_pick::sector state", [&] {
		std::mt19937_64 rng(20261118);
		int missed_by_default = 0;
		for (int i = 0; i < 200; ++i) {
			const auto s = random_sequence(rng, false);
			const auto memory = memory_io_states(s);
			if (!CHECK(covers(faultfs_states(s, sector), memory))) { std::fprintf(stderr, "  sequence %d: durable %zu, %zu appends\n", i, s.durable.size(), s.appends.size()); }
			missed_by_default += !covers(faultfs_states(s, {}, 256), memory);
		}
		// The default size coin does not reach them (F3): the opt-in is what covers MemoryIO.
		CHECK(missed_by_default > 0);
	});
	faultlab_test::run("every CrashIO state over block appends is a size_pick::sector state", [&] {
		std::mt19937_64 rng(20261119);
		for (int i = 0; i < 200; ++i) {
			const auto s = random_sequence(rng, true);
			if (!CHECK(covers(faultfs_states(s, sector), crash_io_states(s)))) { std::fprintf(stderr, "  sequence %d: durable %zu, %zu appends\n", i, s.durable.size(), s.appends.size()); }
		}
	});
	faultlab_test::run("size_pick::sector cuts only at sector boundaries or either size", [&] {
		std::mt19937_64 rng(20261120);
		for (int i = 0; i < 50; ++i) {
			const auto s = random_sequence(rng, false);
			std::size_t current = s.durable.size();
			for (const auto& a : s.appends) { current += a.size(); }
			for (const auto& state : faultfs_states(s, sector, 256)) {
				const auto n = state.size();
				CHECK(n == s.durable.size() || n == current || (n % 512 == 0 && n > s.durable.size() && n < current));
			}
		}
	});
	return faultlab_test::verdict();
}
