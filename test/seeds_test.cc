// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// F3: crash(seed) with the default policy rebuilds the state strata's faultfs rebuilt for that
// seed, for every recorded scenario and seed (test/strata_seeds.inc, from strata:491a1bd).
#include "faultlab/faultfs.h"

#include "check.h"
#include "seed_scenarios.h"

#include <cinttypes>

namespace {

struct entry {
	int scenario;
	std::uint64_t seed, fingerprint;
};
const entry table[] = {
#include "strata_seeds.inc"
};

}  // namespace

int main() {
	namespace ff = kronuz::faultlab;
	faultlab_test::run("every recorded strata seed rebuilds its state", [] {
		CHECK(std::size(table) == std::size_t(seed_scenarios::count) * seed_scenarios::seeds);
		for (const auto& e : table) {
			ff::filesystem fs;
			seed_scenarios::build<ff::filesystem, ff::io, ff::fault, ff::op>(e.scenario, fs);
			fs.crash(e.seed);
			const auto got = seed_scenarios::fingerprint(fs);
			if (!CHECK(got == e.fingerprint)) {
				std::fprintf(stderr, "  scenario %d seed %" PRIu64 ": 0x%016" PRIx64 ", recorded 0x%016" PRIx64 "\n", e.scenario, e.seed, got, e.fingerprint);
			}
		}
	});
	faultlab_test::run("an explicit coin policy is the default", [] {
		for (const auto& e : table) {
			ff::filesystem fs;
			seed_scenarios::build<ff::filesystem, ff::io, ff::fault, ff::op>(e.scenario, fs);
			fs.crash(e.seed, ff::crash_policy{});
			CHECK(seed_scenarios::fingerprint(fs) == e.fingerprint);
		}
	});
	return faultlab_test::verdict();
}
