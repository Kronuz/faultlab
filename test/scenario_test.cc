// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The scenario line (F11): printing, parsing, refusals, and FAULTLAB_SCENARIO's selection.
#include "faultlab/scenario.h"

#include "check.h"

#include <cstdlib>

namespace ff = kronuz::faultlab;

namespace {

bool refused(std::string_view line) {
	try {
		ff::scenario::parse(line);
	} catch (const std::invalid_argument&) {
		return true;
	}
	return false;
}

}  // namespace

int main() {
	using faultlab_test::run;
	run("a line prints and parses back", [] {
		ff::scenario s;
		s.name = "mirror_crash.first";
		s.seed = 11;
		s.cut = 513;
		s.pick = ff::crash_policy{ff::epoch_pick::all, ff::name_pick::additions, ff::size_pick::current, ff::sector_pick::old};
		s.extra = {{"store", "relay"}};
		const std::string line = s.line();
		CHECK(line == "faultlab/1 name=mirror_crash.first seed=11 cut=513 pick=all,additions,current,old store=relay");
		const auto back = ff::scenario::parse(line);
		CHECK(back.line() == line);
		CHECK(back.name == s.name && back.seed == 11 && back.cut == 513u);
		CHECK(back.pick->names == ff::name_pick::additions && back.pick->sectors == ff::sector_pick::old);
		CHECK(back.get("store") == std::optional<std::string>("relay"));
		CHECK(!back.get("missing"));
		CHECK(s.replay() == "FAULTLAB_SCENARIO='" + line + "'");
		CHECK(ff::scenario::parse("faultlab/1  seed=18446744073709551615\tname=x ").seed == 18446744073709551615ull);
	});
	run("the seed drives one stream", [] {
		ff::scenario a, b;
		a.seed = b.seed = 42;
		auto ra = a.random(), rb = b.random();
		CHECK(ra() == rb());
	});
	run("malformed lines are refused", [] {
		CHECK(refused(""));
		CHECK(refused("faultlab/2 name=x seed=1"));
		CHECK(refused("faultlab/1 seed=1"));
		CHECK(refused("faultlab/1 name=x"));
		CHECK(refused("faultlab/1 name=x seed=-1"));
		CHECK(refused("faultlab/1 name=x seed=18446744073709551616"));
		CHECK(refused("faultlab/1 name=x seed=1 seed=2"));
		CHECK(refused("faultlab/1 name=x seed=1 pick=all,all,all"));
		CHECK(refused("faultlab/1 name=x seed=1 pick=all,all,all,new"));
		CHECK(refused("faultlab/1 name=x seed=1 loose"));
		CHECK(refused("faultlab/1 name=x seed=1 k=1 k=2"));
		CHECK(refused("faultlab/1 name=x seed=1 k=a=b"));
		CHECK(refused("faultlab/1 name=a=b seed=1"));
		ff::scenario bad;
		bad.name = "two words";
		bool threw = false;
		try {
			(void)bad.line();
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		CHECK(threw);
	});
	run("FAULTLAB_SCENARIO selects exactly its case", [] {
		ff::scenario a, b;
		a.name = b.name = "case";
		a.seed = 1;
		b.seed = 2;
		::unsetenv("FAULTLAB_SCENARIO");
		CHECK(a.selected() && b.selected());
		::setenv("FAULTLAB_SCENARIO", "faultlab/1 name=case seed=2", 1);
		CHECK(!a.selected() && b.selected());
		b.cut = 7;
		CHECK(b.selected());  // the line names no cut
		::setenv("FAULTLAB_SCENARIO", "faultlab/1 name=case seed=2 cut=8", 1);
		CHECK(!b.selected());
		::unsetenv("FAULTLAB_SCENARIO");
	});
	return faultlab_test::verdict();
}
