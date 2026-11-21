// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// size_pick::sector: every state journal's MemoryIO (its power_loss) and storage's CrashIO
// (test_crash.cc's reconstruct: a prefix of the writes, the next one torn to its first 512 bytes)
// leave after random unsynced appends to a synced file is a state faultfs reaches with
// size_pick::sector. CrashIO's appends are whole blocks, as storage writes them. MemoryIO's states
// are recorded in fixtures/memory_io_states.txt (journal a412175); CrashIO's are computed here.
#include "faultlab/faultfs.h"

#include "check.h"
#include "fixtures/sector_sequences.h"

#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ff = kronuz::faultlab;
using faultlab_test::random_bytes;
using faultlab_test::random_sequence;
using faultlab_test::sequence;

namespace {

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

// The recorded MemoryIO states of each of the fixture's sequences; a sequence whose shape differs
// from the fixture's throws.
std::vector<std::set<std::string>> recorded_memory_io_states(const std::vector<sequence>& cases) {
	std::ifstream in(FAULTLAB_FIXTURES_DIR "/memory_io_states.txt");
	if (!in) { throw std::runtime_error("memory_io_states.txt: not readable"); }
	std::vector<std::set<std::string>> out;
	const sequence* current = nullptr;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') { continue; }
		std::istringstream words(line);
		std::string kind;
		words >> kind;
		if (kind == "sequence") {
			std::size_t n = 0, durable = 0;
			std::string durable_word, appends_word;
			words >> n >> durable_word >> durable >> appends_word;
			if (n != out.size() || n >= cases.size()) { throw std::runtime_error("memory_io_states.txt: sequence out of order: " + line); }
			current = &cases[n];
			std::size_t size = 0;
			std::size_t count = 0;
			bool same = durable == current->durable.size();
			while (words >> size) { same = same && count < current->appends.size() && size == current->appends[count++].size(); }
			if (!same || count != current->appends.size()) { throw std::runtime_error("memory_io_states.txt: sequence shape differs from the test's: " + line); }
			out.emplace_back();
		} else if (kind == "state" && current) {
			std::size_t size = 0;
			words >> size;
			std::vector<faultlab_test::zero_range> zeros;
			std::string word;
			words >> word;  // "zero" or nothing
			for (std::size_t from, to; words >> from && words.get() == '-' && words >> to;) { zeros.push_back({from, to}); }
			out.back().insert(faultlab_test::expand(*current, size, zeros));
		} else {
			throw std::runtime_error("memory_io_states.txt: unreadable line: " + line);
		}
	}
	if (out.size() != cases.size()) { throw std::runtime_error("memory_io_states.txt: wrong sequence count"); }
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
		const auto cases = faultlab_test::memory_io_cases();
		const auto recorded = recorded_memory_io_states(cases);
		int missed_by_default = 0;
		for (int i = 0; i < int(cases.size()); ++i) {
			const auto& s = cases[i];
			const auto& memory = recorded[i];
			if (!CHECK(covers(faultfs_states(s, sector), memory))) { std::fprintf(stderr, "  sequence %d: durable %zu, %zu appends\n", i, s.durable.size(), s.appends.size()); }
			missed_by_default += !covers(faultfs_states(s, {}, 256), memory);
		}
		// The default size coin does not reach them (F3): the opt-in is what covers MemoryIO.
		CHECK(missed_by_default > 0);
	});
	faultlab_test::run("every CrashIO state over block appends is a size_pick::sector state", [&] {
		std::mt19937_64 rng(20261119);
		for (int i = 0; i < 100; ++i) {
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
