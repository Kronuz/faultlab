// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// Records the states journal's MemoryIO (its power_loss) leaves for each sequence sector_test
// checks, as test/fixtures/memory_io_states.txt. Built only with FAULTLAB_REGENERATE_MEMORY_IO,
// which fetches journal at FAULTLAB_JOURNAL_COMMIT; usage: faultlab_memory_io_gen OUT.
#include "sector_sequences.h"

#include "journal/memory_io.h"

#include <cstdio>
#include <fstream>
#include <set>

namespace mem = kronuz::journal::testing;
using namespace faultlab_test;

namespace {

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

// The zero ranges of `state` against the image; fails when a byte is neither the image's nor zero.
bool zero_ranges(const sequence& s, const std::string& state, std::vector<zero_range>& out) {
	const auto image = s.image();
	if (state.size() > image.size()) { return false; }
	for (std::size_t i = 0; i < state.size(); ++i) {
		if (state[i] == image[i]) { continue; }
		if (state[i] != '\0') { return false; }
		if (!out.empty() && out.back().to == i) { ++out.back().to; } else { out.push_back({i, i + 1}); }
	}
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s OUT\n", argv[0]);
		return 2;
	}
	std::ofstream out(argv[1], std::ios::binary);
	out << "# journal's MemoryIO power_loss states for the sequences test/sector_test.cc checks.\n"
	    << "# source: journal " FAULTLAB_JOURNAL_COMMIT << " include/journal/memory_io.h\n"
	    << "# regenerate: cmake -DFAULTLAB_REGENERATE_MEMORY_IO=ON, build faultlab_memory_io_gen, run it with an output path\n"
	    << "# format: `sequence N durable D appends A1 A2..` (byte counts, the bytes come from sector_sequences.h);\n"
	    << "# then one `state SIZE [zero FROM-TO..]` per state: the sequence's full image cut to SIZE with\n"
	    << "# the ranges [FROM, TO) zero (a lost sector). Sorted by state bytes.\n";
	int n = 0;
	for (const auto& s : memory_io_cases()) {
		out << "sequence " << n++ << " durable " << s.durable.size() << " appends";
		for (const auto& a : s.appends) { out << ' ' << a.size(); }
		out << '\n';
		for (const auto& state : memory_io_states(s)) {
			std::vector<zero_range> zeros;
			if (!zero_ranges(s, state, zeros) || expand(s, state.size(), zeros) != state) {
				std::fprintf(stderr, "sequence %d: a MemoryIO state is not the image with zero ranges\n", n - 1);
				return 1;
			}
			out << "state " << state.size();
			if (!zeros.empty()) {
				out << " zero";
				for (const auto& z : zeros) { out << ' ' << z.from << '-' << z.to; }
			}
			out << '\n';
		}
	}
	out.flush();
	return out ? 0 : 1;
}
