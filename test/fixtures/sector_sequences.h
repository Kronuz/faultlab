// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The append sequences sector_test checks and the recorded MemoryIO states of each, shared with
// the generator that records them (memory_io_gen.cc).
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace faultlab_test {

struct sequence {
	std::string durable;
	std::vector<std::string> appends;

	// The file with every append written: a recorded state is a prefix of it with zero ranges.
	std::string image() const {
		std::string out = durable;
		for (const auto& a : appends) { out += a; }
		return out;
	}
};

inline std::string random_bytes(std::mt19937_64& rng, std::size_t n) {
	std::string out(n, '\0');
	for (auto& c : out) { c = char('a' + rng() % 26); }
	return out;
}

// Appends of 1 to 4 dirty sectors over a synced file, so 2048 seeds reach every faultfs state.
inline sequence random_sequence(std::mt19937_64& rng, bool blocks) {
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

// The sequences of the MemoryIO case, in the fixture's order.
constexpr std::uint64_t memory_io_seed = 20261118;
constexpr int memory_io_sequences = 100;

inline std::vector<sequence> memory_io_cases() {
	std::mt19937_64 rng(memory_io_seed);
	std::vector<sequence> out;
	for (int i = 0; i < memory_io_sequences; ++i) { out.push_back(random_sequence(rng, false)); }
	return out;
}

// A recorded state: its size and the byte ranges [from, to) that are zero; every other byte is
// the sequence's image.
struct zero_range {
	std::size_t from, to;
};

inline std::string expand(const sequence& s, std::size_t size, const std::vector<zero_range>& zeros) {
	std::string out = s.image().substr(0, size);
	for (const auto& z : zeros) { std::fill(out.begin() + z.from, out.begin() + z.to, '\0'); }
	return out;
}

}  // namespace faultlab_test
