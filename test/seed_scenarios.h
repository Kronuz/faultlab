// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The scenarios behind the recorded seed table (F3): each builds a state through strata's faultfs
// API, which faultlab keeps, then crashes it with a seed. `ff` names the implementation: strata's
// faultfs for test/tools/strata_seeds.cc, faultlab for seeds_test.cc.
#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace seed_scenarios {

inline constexpr int count = 6;
inline constexpr std::uint64_t seeds = 64;

inline std::string bytes(std::size_t n, char fill) {
	std::string out(n, fill);
	for (std::size_t i = 0; i < n; i += 97) { out[i] = char('a' + i % 26); }
	return out;
}

// FNV-1a over every name and its bytes, in name order.
template <class Filesystem>
std::uint64_t fingerprint(Filesystem& fs) {
	std::uint64_t h = 14695981039346656037ull;
	auto mix = [&](std::string_view s) {
		for (unsigned char c : s) { h = (h ^ c) * 1099511628211ull; }
		h = (h ^ 0xff) * 1099511628211ull;
	};
	for (const auto& name : fs.list()) {
		mix(name);
		mix(fs.view(name));
	}
	return h;
}

template <class Filesystem, class IO, class Fault, class Op>
void build(int which, Filesystem& fs) {
	IO io(fs);
	switch (which) {
	case 0: {  // one file: synced bytes, then overwrites and an append
		auto f = io.create_exclusive("data");
		io.sync_directory();
		f->write_at(0, bytes(1500, 'd'));
		f->sync();
		f->write_at(100, bytes(700, 'o'));
		f->write_at(1500, bytes(1500, 'a'));
		break;
	}
	case 1: {  // the namespace: renames, removes and creates after a directory sync
		for (const char* n : {"a", "b", "c"}) {
			auto f = io.create_exclusive(n);
			f->write_at(0, bytes(600, *n));
			f->sync();
		}
		io.sync_directory();
		io.replace("a", "d");
		io.remove("b");
		auto e = io.create_exclusive("e");
		e->write_at(0, bytes(900, 'e'));
		e->sync();
		io.replace("e", "c");
		auto g = io.create_exclusive("g");
		g->write_at(0, bytes(300, 'g'));
		break;
	}
	case 2: {  // ordering points: two chained epochs and writes after them
		auto f = io.create_exclusive("log");
		f->write_at(0, bytes(1024, 'l'));
		kronuz::journal::File* files[] = {f.get()};
		io.order_files_and_directory(files);
		auto t = io.create_exclusive("tmp");
		t->write_at(0, bytes(700, 't'));
		kronuz::journal::File* both[] = {f.get(), t.get()};
		f->write_at(1024, bytes(512, 'm'));
		io.order_files_and_directory(both);
		io.replace("tmp", "manifest");
		f->write_at(1536, bytes(800, 'n'));
		io.remove("log");
		break;
	}
	case 3: {  // a failed sync that keeps its pages, then more writes
		auto f = io.create_exclusive("kept");
		io.sync_directory();
		f->write_at(0, bytes(8192, 'k'));
		f->sync();
		fs.add_fault(Fault{Op::sync, 1, 5, false, true});
		f->write_at(4096, bytes(2000, 'x'));
		try {
			f->sync();
		} catch (...) {
		}
		f->write_at(0, bytes(100, 'y'));
		break;
	}
	case 4: {  // truncation: shrink, grow and a sync that fails after its effect
		auto f = io.create_exclusive("trunc");
		f->write_at(0, bytes(3000, 'r'));
		f->sync();
		io.sync_directory();
		f->truncate(1000);
		f->write_at(2000, bytes(600, 's'));
		fs.add_fault(Fault{Op::sync, 1, 5, true, false});
		try {
			f->sync();
		} catch (...) {
		}
		f->truncate(2900);
		break;
	}
	default: {  // many files and mixed operations from a fixed stream
		std::mt19937_64 rng(20261118);
		std::vector<std::string> live;
		std::vector<std::unique_ptr<kronuz::journal::File>> handles;
		int next = 0;
		for (int step = 0; step < 80; ++step) {
			const auto pick = rng() % 10;
			if (pick < 2 || live.empty()) {
				live.push_back("f" + std::to_string(next++));
				handles.push_back(io.create_exclusive(live.back()));
			} else if (pick < 5) {
				auto& h = handles[rng() % handles.size()];
				h->write_at(rng() % 2000, bytes(1 + rng() % 1200, char('A' + rng() % 26)));
			} else if (pick < 6) {
				handles[rng() % handles.size()]->sync();
			} else if (pick < 7) {
				io.sync_directory();
			} else if (pick < 8) {
				std::vector<kronuz::journal::File*> some;
				for (auto& h : handles) {
					if (rng() % 2) { some.push_back(h.get()); }
				}
				io.order_files_and_directory(some);
			} else if (pick < 9) {
				const auto i = rng() % live.size();
				const std::string to = "r" + std::to_string(next++);
				io.replace(live[i], to);
				live[i] = to;
			} else {
				const auto i = rng() % live.size();
				io.remove(live[i]);
				live.erase(live.begin() + std::ptrdiff_t(i));
			}
		}
		break;
	}
	}
}

}  // namespace seed_scenarios
