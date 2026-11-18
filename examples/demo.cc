// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// Publishes a file with write, sync, link and unlink, then sweeps every crash point and a few
// seeds of each: recovery must find the old version or the new one, never a torn mix.
#include "faultlab/faultfs.h"
#include "faultlab/scenario.h"

#include <cstdio>
#include <string>

namespace ff = kronuz::faultlab;

// The program under test: write a temporary, make it durable, publish it under `name`.
static void publish(kronuz::journal::IO& io, const std::string& name, const std::string& bytes) {
	auto tmp = io.create_exclusive(name + ".tmp");
	for (std::size_t at = 0; at < bytes.size();) { at += tmp->write_at(at, std::string_view(bytes).substr(at)); }
	tmp->sync();
	io.sync_directory();
	io.remove(name);
	io.link(name + ".tmp", name);
	io.remove(name + ".tmp");
	io.sync_directory();
}

int main() {
	const std::string old_bytes(3000, 'o'), new_bytes(5000, 'n');
	ff::filesystem base;
	{
		ff::io io(base);
		publish(io, "doc", old_bytes);
	}
	// Count the new publication's mutating operations, to cut after each one.
	auto probe = base.snapshot();
	const auto before = probe.mutations();
	{
		ff::io io(probe);
		publish(io, "doc", new_bytes);
	}
	const auto steps = probe.mutations() - before;

	int failures = 0;
	for (std::uint64_t cut = 0; cut <= steps; ++cut) {
		for (std::uint64_t seed = 0; seed < 32; ++seed) {
			ff::scenario s{"demo.publish", seed, cut, std::nullopt, {}};
			if (!s.selected()) { continue; }
			auto fs = base.snapshot();
			fs.power_off_after(cut);
			try {
				ff::io io(fs);
				publish(io, "doc", new_bytes);
			} catch (const std::system_error&) {
				// the power went off
			}
			fs.crash(seed);
			std::string got;
			try {
				got = std::string(fs.view("doc"));
			} catch (const std::system_error&) {
				got = std::string(fs.view("doc.tmp"));  // the publication's window: only the temporary
			}
			if (got != old_bytes && got != new_bytes) {
				++failures;
				std::printf("torn: %zu bytes; replay with %s\n", got.size(), s.replay().c_str());
			}
		}
	}
	std::printf("%llu crash points x 32 seeds: %s\n", (unsigned long long)(steps + 1), failures ? "TORN" : "old or new, never torn");
	return failures ? 1 : 0;
}
