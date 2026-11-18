// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The JSON-line event log (F12): one file per run, one object per line, which
// python/faultlab/history.py checks.
//
//   {"scenario": "faultlab/1 name=... seed=..."}                     the first line
//   {"client": "c1", "op": "write", "key": "k", "value": "v1", "level": "quorum",
//    "outcome": "invoke", "time": 0.5}                                  operations
//   {"fault": "partition", "time": 1.0, "nodes": ["n1", "n2"]}         faults
//   {"kind": "phase", ...}                                              harness records, set aside
//
// `time` is seconds since the run started, or a logical clock; the checker needs it
// non-decreasing per client. Each line is flushed as it is written.
#pragma once

#include "faultlab/scenario.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace kronuz::faultlab {

// A JSON value as the log writes it: a scalar, or a list of strings (a fault's nodes).
using json_value = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, std::vector<std::string>>;
using json_fields = std::vector<std::pair<std::string, json_value>>;

// One invocation or completion, with the fields history.py reads.
struct operation {
	std::string client;
	std::string op;  // write, read or get
	std::string key;
	json_value value = nullptr;
	std::string level;    // write: quorum or none; read: prefix, linearizable or seal
	std::string outcome;  // invoke, ok, fail or info
	double time = 0;
	std::optional<std::string> node;  // the node that served or acknowledged it
	std::optional<std::int64_t> launch;  // that node's launch, counted from 1
	bool durable = false;  // the node keeps what it served across launches (the monotonic check)
};

class event_log {
public:
	static constexpr std::array<std::string_view, 16> fault_kinds{
		"partition", "heal", "isolate", "reset", "cap", "delay", "loss", "reorder", "duplicate",  // network
		"pause", "resume", "kill", "restart",  // process
		"power_cut", "media", "enospc",  // disk
	};

	// Creates or truncates `path` and writes the scenario line.
	event_log(const std::filesystem::path& path, const scenario& s) : start_(std::chrono::steady_clock::now()) {
		file_ = std::fopen(path.string().c_str(), "wb");
		if (!file_) { throw std::system_error(errno, std::generic_category(), "faultlab event log: " + path.string()); }
		write({{"scenario", s.line()}});
	}
	~event_log() {
		if (file_) { std::fclose(file_); }
	}
	event_log(const event_log&) = delete;
	event_log& operator=(const event_log&) = delete;

	// Seconds since the log opened, for `time` when it is the wall clock.
	double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }

	void record(const operation& o) {
		if (o.op != "write" && o.op != "read" && o.op != "get") { throw std::invalid_argument("faultlab event log: unknown op " + o.op); }
		if (o.outcome != "invoke" && o.outcome != "ok" && o.outcome != "fail" && o.outcome != "info") { throw std::invalid_argument("faultlab event log: unknown outcome " + o.outcome); }
		json_fields line{{"client", o.client}, {"op", o.op}, {"key", o.key}, {"value", o.value}, {"level", o.level}, {"outcome", o.outcome}, {"time", o.time}};
		if (o.node) { line.emplace_back("node", *o.node); }
		if (o.launch) { line.emplace_back("launch", *o.launch); }
		if (o.durable) { line.emplace_back("durable", true); }
		write(line);
	}
	// A fault of `kind` (fault_kinds) at `time`, its arguments by name: nodes, ms, p, seed, at.
	void fault(std::string_view kind, double time, json_fields args = {}) {
		bool known = false;
		for (auto k : fault_kinds) { known = known || k == kind; }
		if (!known) { throw std::invalid_argument("faultlab event log: unknown fault " + std::string(kind)); }
		json_fields line{{"fault", std::string(kind)}, {"time", time}};
		for (auto& f : args) {
			if (f.first == "fault" || f.first == "time" || f.first == "kind") { throw std::invalid_argument("faultlab event log: reserved field " + f.first); }
			line.push_back(std::move(f));
		}
		write(line);
	}
	// A harness record: its own line, which the checker sets aside.
	void note(std::string_view kind, json_fields fields = {}) {
		json_fields line{{"kind", std::string(kind)}};
		for (auto& f : fields) {
			if (f.first == "kind") { throw std::invalid_argument("faultlab event log: reserved field kind"); }
			line.push_back(std::move(f));
		}
		write(line);
	}

	// One object as a JSON line, keys in the given order.
	static std::string encode(const json_fields& fields) {
		std::string out = "{";
		bool first = true;
		for (const auto& [key, value] : fields) {
			if (!first) { out += ", "; }
			first = false;
			quote(out, key);
			out += ": ";
			encode(out, value);
		}
		out += "}";
		return out;
	}

private:
	static void quote(std::string& out, std::string_view s) {
		out += '"';
		for (unsigned char c : s) {
			switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof buf, "\\u%04x", c);
					out += buf;
				} else {
					out += char(c);
				}
			}
		}
		out += '"';
	}
	static void encode(std::string& out, const json_value& v) {
		if (std::holds_alternative<std::nullptr_t>(v)) {
			out += "null";
		} else if (auto* b = std::get_if<bool>(&v)) {
			out += *b ? "true" : "false";
		} else if (auto* i = std::get_if<std::int64_t>(&v)) {
			out += std::to_string(*i);
		} else if (auto* d = std::get_if<double>(&v)) {
			if (!std::isfinite(*d)) { throw std::invalid_argument("faultlab event log: a number must be finite"); }
			char buf[32];
			std::snprintf(buf, sizeof buf, "%.17g", *d);
			out += buf;
			if (std::string_view(buf).find_first_of(".eE") == std::string_view::npos) { out += ".0"; }
		} else if (auto* s = std::get_if<std::string>(&v)) {
			quote(out, *s);
		} else {
			out += "[";
			bool first = true;
			for (const auto& s : std::get<std::vector<std::string>>(v)) {
				if (!first) { out += ", "; }
				first = false;
				quote(out, s);
			}
			out += "]";
		}
	}
	void write(const json_fields& fields) {
		const std::string line = encode(fields) + "\n";
		std::lock_guard lock(mu_);
		if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() || std::fflush(file_) != 0) {
			throw std::system_error(errno, std::generic_category(), "faultlab event log: write");
		}
	}

	std::FILE* file_ = nullptr;
	std::chrono::steady_clock::time_point start_;
	std::mutex mu_;
};

}  // namespace kronuz::faultlab
