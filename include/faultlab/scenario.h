// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The seeded scenario line (F11). A test derives all its randomness from one seed and, on
// failure, prints one line that replays it:
//
//   faultlab/1 name=mirror_crash.first seed=11 cut=513 pick=all,additions,current,old
//
// Space-separated key=value after the version; `name` and `seed` required; `cut` (the
// power_off_after count), `pick` (a crash_policy, in field order) and further keys optional.
// FAULTLAB_SCENARIO=<line> runs exactly that case. python/faultlab/scenario.py reads and writes
// the same line.
#pragma once

#include "faultlab/faultfs.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kronuz::faultlab {

class scenario {
public:
	static constexpr std::string_view version = "faultlab/1";

	std::string name;
	std::uint64_t seed = 0;
	std::optional<std::uint64_t> cut;
	std::optional<crash_policy> pick;
	std::vector<std::pair<std::string, std::string>> extra;  // further keys, in order

	// The one random stream every choice of the case draws from.
	std::mt19937_64 random() const { return std::mt19937_64(seed); }

	std::string line() const {
		check_word("name", name);
		std::string out(version);
		out += " name=" + name + " seed=" + std::to_string(seed);
		if (cut) { out += " cut=" + std::to_string(*cut); }
		if (pick) {
			out += " pick=";
			out += epochs_[std::size_t(pick->epochs)];
			out += ',';
			out += names_[std::size_t(pick->names)];
			out += ',';
			out += sizes_[std::size_t(pick->size)];
			out += ',';
			out += sectors_[std::size_t(pick->sectors)];
		}
		for (const auto& [key, value] : extra) {
			check_word("key", key);
			check_word(key.c_str(), value);
			if (reserved(key)) { throw std::invalid_argument("faultlab scenario: extra key " + key + " is reserved"); }
			out += " " + key + "=" + value;
		}
		return out;
	}
	// The command-line prefix that replays this case.
	std::string replay() const { return "FAULTLAB_SCENARIO='" + line() + "'"; }

	static scenario parse(std::string_view line) {
		auto words = split(line);
		if (words.empty() || words[0] != version) { throw std::invalid_argument("faultlab scenario: not a " + std::string(version) + " line"); }
		scenario out;
		bool has_name = false, has_seed = false;
		for (std::size_t i = 1; i < words.size(); ++i) {
			const auto eq = words[i].find('=');
			if (eq == std::string_view::npos || eq == 0) { throw std::invalid_argument("faultlab scenario: not key=value: " + std::string(words[i])); }
			const std::string key(words[i].substr(0, eq)), value(words[i].substr(eq + 1));
			if (value.empty()) { throw std::invalid_argument("faultlab scenario: empty " + key); }
			if (value.find('=') != std::string::npos) { throw std::invalid_argument("faultlab scenario: '=' inside " + key); }  // line() could not print it back
			auto seen = [&](bool& flag) {
				if (flag) { throw std::invalid_argument("faultlab scenario: " + key + " twice"); }
				flag = true;
			};
			if (key == "name") {
				seen(has_name);
				out.name = value;
			} else if (key == "seed") {
				seen(has_seed);
				out.seed = number(key, value);
			} else if (key == "cut") {
				if (out.cut) { throw std::invalid_argument("faultlab scenario: cut twice"); }
				out.cut = number(key, value);
			} else if (key == "pick") {
				if (out.pick) { throw std::invalid_argument("faultlab scenario: pick twice"); }
				out.pick = parse_pick(value);
			} else {
				for (const auto& [k, _] : out.extra) {
					if (k == key) { throw std::invalid_argument("faultlab scenario: " + key + " twice"); }
				}
				out.extra.emplace_back(key, value);
			}
		}
		if (!has_name || !has_seed) { throw std::invalid_argument("faultlab scenario: name and seed are required"); }
		return out;
	}

	// The case FAULTLAB_SCENARIO names, when it is set.
	static std::optional<scenario> requested() {
		const char* env = std::getenv("FAULTLAB_SCENARIO");
		if (!env || !*env) { return std::nullopt; }
		return parse(env);
	}
	// Whether this case runs: every case without FAULTLAB_SCENARIO, else only the case whose
	// name and seed it names (and cut and pick, when it names them).
	bool selected() const {
		const auto want = requested();
		if (!want) { return true; }
		return want->name == name && want->seed == seed && (!want->cut || want->cut == cut) && (!want->pick || line_pick(*want) == line_pick(*this));
	}
	// The value of an extra key, if present.
	std::optional<std::string> get(std::string_view key) const {
		for (const auto& [k, v] : extra) {
			if (k == key) { return v; }
		}
		return std::nullopt;
	}

private:
	static constexpr std::array<const char*, 3> epochs_{"coin", "none", "all"};
	static constexpr std::array<const char*, 4> names_{"coin", "durable", "all", "additions"};
	static constexpr std::array<const char*, 4> sizes_{"coin", "durable", "current", "sector"};
	static constexpr std::array<const char*, 3> sectors_{"coin", "old", "fresh"};

	static bool reserved(std::string_view key) { return key == "name" || key == "seed" || key == "cut" || key == "pick"; }
	static void check_word(const char* what, std::string_view word) {
		if (word.empty() || word.find_first_of(" \t\n\r=") != std::string_view::npos) {
			throw std::invalid_argument(std::string("faultlab scenario: ") + what + " must be one word without '='");
		}
	}
	static std::vector<std::string_view> split(std::string_view line) {
		std::vector<std::string_view> out;
		std::size_t at = 0;
		while (at < line.size()) {
			const auto begin = line.find_first_not_of(" \t\r\n", at);
			if (begin == std::string_view::npos) { break; }
			const auto end = line.find_first_of(" \t\r\n", begin);
			out.push_back(line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
			at = end == std::string_view::npos ? line.size() : end;
		}
		return out;
	}
	static std::uint64_t number(const std::string& key, const std::string& value) {
		if (value.find_first_not_of("0123456789") != std::string::npos || value.size() > 20) { throw std::invalid_argument("faultlab scenario: " + key + " is not a number: " + value); }
		std::uint64_t n = 0;
		for (char c : value) {
			const std::uint64_t digit = std::uint64_t(c - '0');
			if (n > (~std::uint64_t(0) - digit) / 10) { throw std::invalid_argument("faultlab scenario: " + key + " overflows: " + value); }
			n = n * 10 + digit;
		}
		return n;
	}
	template <std::size_t N>
	static std::size_t index(const std::array<const char*, N>& names, std::string_view word, const char* field) {
		for (std::size_t i = 0; i < N; ++i) {
			if (word == names[i]) { return i; }
		}
		throw std::invalid_argument(std::string("faultlab scenario: unknown ") + field + " pick " + std::string(word));
	}
	static crash_policy parse_pick(std::string_view value) {
		std::vector<std::string_view> parts;
		std::size_t at = 0;
		for (;;) {
			const auto comma = value.find(',', at);
			parts.push_back(value.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
			if (comma == std::string_view::npos) { break; }
			at = comma + 1;
		}
		if (parts.size() != 4) { throw std::invalid_argument("faultlab scenario: pick takes epochs,names,size,sectors"); }
		crash_policy p;
		p.epochs = epoch_pick(index(epochs_, parts[0], "epochs"));
		p.names = name_pick(index(names_, parts[1], "names"));
		p.size = size_pick(index(sizes_, parts[2], "size"));
		p.sectors = sector_pick(index(sectors_, parts[3], "sectors"));
		return p;
	}
	static std::optional<std::array<int, 4>> line_pick(const scenario& s) {
		if (!s.pick) { return std::nullopt; }
		return std::array<int, 4>{int(s.pick->epochs), int(s.pick->names), int(s.pick->size), int(s.pick->sectors)};
	}
};

}  // namespace kronuz::faultlab
