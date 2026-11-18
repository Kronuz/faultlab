// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The event log (F12): its lines, exactly. With a path argument it writes a small register
// history there, which the checker test (python/tests/events_check.py) then checks.
#include "faultlab/events.h"

#include "check.h"

#include <fstream>
#include <limits>

namespace ff = kronuz::faultlab;

namespace {

std::vector<std::string> lines(const std::filesystem::path& path) {
	std::ifstream in(path);
	std::vector<std::string> out;
	for (std::string line; std::getline(in, line);) { out.push_back(line); }
	return out;
}

ff::scenario scenario() {
	ff::scenario s;
	s.name = "events";
	s.seed = 3;
	return s;
}

// A register history with one partition: a write, a read of it, and a write that failed. A stale
// one then reads the first value after a newer write completed, during a second partition.
void history(ff::event_log& log, bool stale = false) {
	auto op = [&](const char* client, const char* kind, ff::json_value value, const char* level, const char* outcome, double time) {
		log.record({client, kind, "k", std::move(value), level, outcome, time, "n1", 1, false});
	};
	log.note("phase", {{"name", std::string("start")}});
	op("w", "write", std::string("v1"), "quorum", "invoke", 0.0);
	op("w", "write", std::string("v1"), "quorum", "ok", 1.0);
	log.fault("partition", 1.5, {{"nodes", std::vector<std::string>{"n1", "n2"}}});
	op("r", "get", nullptr, "linearizable", "invoke", 2.0);
	op("r", "get", std::string("v1"), "linearizable", "ok", 3.0);
	op("w", "write", std::string("v2"), "quorum", "invoke", 3.5);
	op("w", "write", std::string("v2"), "quorum", "fail", 4.0);
	log.fault("heal", 5.0);
	if (stale) {
		op("w", "write", std::string("v3"), "quorum", "invoke", 6.0);
		op("w", "write", std::string("v3"), "quorum", "ok", 7.0);
		log.fault("partition", 7.5, {{"nodes", std::vector<std::string>{"n1", "n3"}}});
		op("r", "get", nullptr, "linearizable", "invoke", 8.0);
		op("r", "get", std::string("v1"), "linearizable", "ok", 9.0);
	}
}

}  // namespace

int main(int argc, char** argv) {
	if (argc >= 2) {
		ff::event_log log(argv[1], scenario());
		history(log, argc == 3 && std::string_view(argv[2]) == "stale");
		return 0;
	}
	using faultlab_test::run;
	run("the log writes the scenario, operations, faults and notes", [] {
		const std::filesystem::path path = ".scratch/events_test.jsonl";
		std::filesystem::create_directories(path.parent_path());
		{
			ff::event_log log(path, scenario());
			history(log);
			ff::operation o{"c", "read", "k", std::int64_t(7), "seal", "ok", 0.25, std::nullopt, std::nullopt, false};
			o.durable = true;
			log.record(o);
		}
		const auto got = lines(path);
		const std::vector<std::string> want{
			R"({"scenario": "faultlab/1 name=events seed=3"})",
			R"({"kind": "phase", "name": "start"})",
			R"({"client": "w", "op": "write", "key": "k", "value": "v1", "level": "quorum", "outcome": "invoke", "time": 0.0, "node": "n1", "launch": 1})",
			R"({"client": "w", "op": "write", "key": "k", "value": "v1", "level": "quorum", "outcome": "ok", "time": 1.0, "node": "n1", "launch": 1})",
			R"({"fault": "partition", "time": 1.5, "nodes": ["n1", "n2"]})",
			R"({"client": "r", "op": "get", "key": "k", "value": null, "level": "linearizable", "outcome": "invoke", "time": 2.0, "node": "n1", "launch": 1})",
			R"({"client": "r", "op": "get", "key": "k", "value": "v1", "level": "linearizable", "outcome": "ok", "time": 3.0, "node": "n1", "launch": 1})",
			R"({"client": "w", "op": "write", "key": "k", "value": "v2", "level": "quorum", "outcome": "invoke", "time": 3.5, "node": "n1", "launch": 1})",
			R"({"client": "w", "op": "write", "key": "k", "value": "v2", "level": "quorum", "outcome": "fail", "time": 4.0, "node": "n1", "launch": 1})",
			R"({"fault": "heal", "time": 5.0})",
			R"({"client": "c", "op": "read", "key": "k", "value": 7, "level": "seal", "outcome": "ok", "time": 0.25, "durable": true})",
		};
		CHECK(got == want);
		for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
			if (got[i] != want[i]) { std::fprintf(stderr, "  line %zu: %s\n", i + 1, got[i].c_str()); }
		}
	});
	run("strings escape; unknown kinds and reserved fields are refused", [] {
		CHECK(ff::event_log::encode({{"s", std::string("a\"b\\c\nd\x01")}}) == R"({"s": "a\"b\\c\nd\u0001"})");
		CHECK(ff::event_log::encode({{"x", 1e300}, {"y", -0.5}}) == R"({"x": 1.0000000000000001e+300, "y": -0.5})");
		ff::event_log log(".scratch/events_refused.jsonl", scenario());
		auto refused = [](auto&& f) {
			try {
				f();
			} catch (const std::invalid_argument&) {
				return true;
			}
			return false;
		};
		CHECK(refused([&] { log.fault("meteor", 0); }));
		CHECK(ff::event_log::encode({{"k", std::string("caf\xc3\xa9 \xf0\x9f\x90\x8d")}}) == "{\"k\": \"caf\xc3\xa9 \xf0\x9f\x90\x8d\"}");  // UTF-8 passes raw
		for (const char* bad : {"\xff", "caf\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80"}) {
			CHECK(refused([&] { log.note("phase", {{"name", std::string(bad)}}); }));
		}
		CHECK(refused([&] { log.fault("kill", 0, {{"time", 1.0}}); }));
		CHECK(refused([&] { log.note("phase", {{"kind", std::string("x")}}); }));
		CHECK(refused([&] { log.record({"c", "delete", "k", nullptr, "quorum", "ok", 0, std::nullopt, std::nullopt, false}); }));
		CHECK(refused([&] { log.record({"c", "write", "k", std::numeric_limits<double>::quiet_NaN(), "quorum", "ok", 0, std::nullopt, std::nullopt, false}); }));
	});
	return faultlab_test::verdict();
}
