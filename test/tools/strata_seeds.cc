// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// Prints the seed table test/strata_seeds.inc records: each scenario's state after crash(seed)
// under strata's faultfs. Built by strata_seeds.sh against strata's core/faultfs.hh.
#include "strata/core/faultfs.hh"

#include "../seed_scenarios.h"

#include <cinttypes>
#include <cstdio>

int main() {
	namespace ff = strata::faultfs;
	for (int s = 0; s < seed_scenarios::count; ++s) {
		for (std::uint64_t seed = 0; seed < seed_scenarios::seeds; ++seed) {
			ff::filesystem fs;
			seed_scenarios::build<ff::filesystem, ff::io, ff::fault, ff::op>(s, fs);
			fs.crash(seed);
			std::printf("{%d, %" PRIu64 ", 0x%016" PRIx64 "ull},\n", s, seed, seed_scenarios::fingerprint(fs));
		}
	}
}
