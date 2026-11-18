// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// F5: each fixed pick's state equals crash() with that field's coins forced (crash_access), the
// other fields' coins drawn from the same seed's stream; and the picks' states, checked directly.
#include "faultlab/faultfs.h"

#include "check.h"
#include "seed_scenarios.h"

#include <map>

namespace ff = kronuz::faultlab;
using site = ff::detail::coin_site;
using state = std::map<std::string, std::string>;

namespace {

// Every file by path, across directories.
state snapshot_state(ff::filesystem& fs, const std::string& directory = {}) {
	state out;
	for (const auto& name : fs.list(directory)) {
		const std::string path = directory.empty() ? name : directory + "/" + name;
		try {
			out[path] = std::string(fs.view(path));
		} catch (const std::system_error&) {
			for (auto& [p, b] : snapshot_state(fs, path)) { out[p] = b; }
		}
	}
	return out;
}

std::string bytes(std::size_t n, char c) { return seed_scenarios::bytes(n, c); }

// A scenario with links, a subdirectory and epochs, on top of the recorded ones.
void build_links(ff::filesystem& fs) {
	fs.make_directory("sub");
	ff::io root(fs), sub(fs, "sub");
	auto a = root.create_exclusive("a");
	a->write_at(0, bytes(900, 'a'));
	a->sync();
	auto s = sub.create_exclusive("s");
	s->write_at(0, bytes(1300, 's'));
	root.sync_directory();
	sub.sync_directory();
	root.link("a", "b");
	kronuz::journal::File* files[] = {a.get(), s.get()};
	a->write_at(512, bytes(700, 'A'));
	root.order_files_and_directory(files);
	root.remove("a");
	sub.link("s", "t");
	s->write_at(1000, bytes(1000, 'S'));
	sub.remove("s");
	auto c = root.create_exclusive("c");
	c->write_at(0, bytes(600, 'c'));
	root.order_files_and_directory(files);
	root.replace("c", "b");
}

struct pick {
	const char* name;
	ff::crash_policy policy;
	ff::detail::coin_force force;
};

std::vector<pick> picks() {
	auto only = [](site s, bool v) { return [s, v](site at, ff::op) -> std::optional<bool> { return at == s ? std::optional<bool>(v) : std::nullopt; }; };
	return {
		{"epochs none", {.epochs = ff::epoch_pick::none}, only(site::epoch, false)},
		{"epochs all", {.epochs = ff::epoch_pick::all}, only(site::epoch, true)},
		{"names durable", {.names = ff::name_pick::durable}, only(site::name, false)},
		{"names all", {.names = ff::name_pick::all}, only(site::name, true)},
		{"names additions", {.names = ff::name_pick::additions},
			[](site at, ff::op kind) -> std::optional<bool> { return at == site::name ? std::optional<bool>(kind != ff::op::remove) : std::nullopt; }},
		{"size durable", {.size = ff::size_pick::durable}, only(site::size, true)},
		{"size current", {.size = ff::size_pick::current}, only(site::size, false)},
		{"sectors old", {.sectors = ff::sector_pick::old}, only(site::sector, false)},
		{"sectors fresh", {.sectors = ff::sector_pick::fresh}, only(site::sector, true)},
	};
}

}  // namespace

int main() {
	std::vector<ff::filesystem> bases;
	for (int s = 0; s < seed_scenarios::count; ++s) {
		ff::filesystem fs;
		seed_scenarios::build<ff::filesystem, ff::io, ff::fault, ff::op>(s, fs);
		bases.push_back(fs.snapshot());
	}
	{
		ff::filesystem fs;
		build_links(fs);
		bases.push_back(fs.snapshot());
	}

	faultlab_test::run("each fixed pick equals crash with its field's coins forced", [&] {
		for (const auto& p : picks()) {
			for (auto& base : bases) {
				for (std::uint64_t seed = 0; seed < 32; ++seed) {
					auto picked = base.snapshot(), forced = base.snapshot();
					picked.crash(seed, p.policy);
					ff::detail::crash_access::crash(forced, seed, p.force);
					if (!CHECK(snapshot_state(picked) == snapshot_state(forced))) { std::fprintf(stderr, "  %s, seed %llu\n", p.name, (unsigned long long)seed); }
				}
			}
		}
	});
	faultlab_test::run("the default policy is crash with no coin forced", [&] {
		for (auto& base : bases) {
			for (std::uint64_t seed = 0; seed < 32; ++seed) {
				auto picked = base.snapshot(), forced = base.snapshot();
				picked.crash(seed);
				ff::detail::crash_access::crash(forced, seed, {});
				CHECK(snapshot_state(picked) == snapshot_state(forced));
			}
		}
	});
	faultlab_test::run("a fixed pick changes the state some seed rebuilds", [&] {
		for (const auto& p : picks()) {
			bool differs = false;
			for (auto& base : bases) {
				for (std::uint64_t seed = 0; seed < 32 && !differs; ++seed) {
					auto picked = base.snapshot(), coin = base.snapshot();
					picked.crash(seed, p.policy);
					coin.crash(seed);
					differs = snapshot_state(picked) != snapshot_state(coin);
				}
			}
			if (!CHECK(differs)) { std::fprintf(stderr, "  %s never differs from the coin\n", p.name); }
		}
	});

	// The picks' states, directly.
	faultlab_test::run("size and sector picks keep the durable or the current bytes", [] {
		const std::string d = bytes(1500, 'd');
		std::string n = d;
		n.replace(100, 700, bytes(700, 'o'));
		n += bytes(1500, 'a');
		auto make = [&] {
			ff::filesystem fs;
			ff::io io(fs);
			auto f = io.create_exclusive("f");
			io.sync_directory();
			f->write_at(0, d);
			f->sync();
			f->write_at(100, bytes(700, 'o'));
			f->write_at(1500, bytes(1500, 'a'));
			return fs.snapshot();
		};
		std::string old_grown = d;
		old_grown.resize(n.size(), '\0');
		const struct {
			ff::size_pick size;
			ff::sector_pick sectors;
			std::string want;
		} cases[] = {
			{ff::size_pick::current, ff::sector_pick::fresh, n},
			{ff::size_pick::durable, ff::sector_pick::old, d},
			{ff::size_pick::durable, ff::sector_pick::fresh, n.substr(0, d.size())},
			{ff::size_pick::current, ff::sector_pick::old, old_grown},
		};
		for (const auto& c : cases) {
			for (std::uint64_t seed = 0; seed < 16; ++seed) {
				auto fs = make();
				fs.crash(seed, {.size = c.size, .sectors = c.sectors});
				CHECK(fs.view("f") == c.want);
			}
		}
	});
	faultlab_test::run("name picks keep the durable, every, or every added name", [] {
		auto make = [] {
			ff::filesystem fs;
			ff::io io(fs);
			io.create_exclusive("a");
			io.create_exclusive("b");
			io.create_exclusive("r");
			io.sync_directory();
			io.create_exclusive("c");
			io.remove("a");
			io.replace("b", "d");
			io.link("d", "e");
			io.remove("r");
			return fs.snapshot();
		};
		using names = std::vector<std::string>;
		const struct {
			ff::name_pick pick;
			names want;
		} cases[] = {
			{ff::name_pick::all, {"c", "d", "e"}},
			{ff::name_pick::durable, {"a", "b", "r"}},
			{ff::name_pick::additions, {"a", "c", "d", "e", "r"}},
		};
		for (const auto& c : cases) {
			for (std::uint64_t seed = 0; seed < 16; ++seed) {
				auto fs = make();
				fs.crash(seed, {.names = c.pick});
				CHECK(fs.list() == c.want);
			}
		}
	});
	faultlab_test::run("epoch picks keep no ordering point or every one", [] {
		const std::string x = bytes(1024, 'x'), y = bytes(512, 'y');
		auto make = [&] {
			ff::filesystem fs;
			ff::io io(fs);
			auto f = io.create_exclusive("f");
			io.sync_directory();
			f->write_at(0, x);
			kronuz::journal::File* files[] = {f.get()};
			io.order_files_and_directory(files);
			io.create_exclusive("g");
			f->write_at(x.size(), y);
			return fs.snapshot();
		};
		for (std::uint64_t seed = 0; seed < 16; ++seed) {
			auto none = make();
			none.crash(seed, {.epochs = ff::epoch_pick::none, .names = ff::name_pick::all, .size = ff::size_pick::current, .sectors = ff::sector_pick::fresh});
			CHECK(none.list() == std::vector<std::string>{"f"});
			CHECK(none.view("f") == x);
			auto all = make();
			all.crash(seed, {.epochs = ff::epoch_pick::all, .names = ff::name_pick::all, .size = ff::size_pick::current, .sectors = ff::sector_pick::fresh});
			CHECK(all.list() == (std::vector<std::string>{"f", "g"}));
			CHECK(all.view("f") == x + y);
		}
	});
	faultlab_test::run("every coin forced to survive keeps the state as it was", [&] {
		for (int s : {0, 1, 2, 4, 5}) {  // scenario 3 loses pages, which no pick keeps
			ff::filesystem fs;
			seed_scenarios::build<ff::filesystem, ff::io, ff::fault, ff::op>(s, fs);
			const auto before = snapshot_state(fs);
			fs.crash(7, {ff::epoch_pick::all, ff::name_pick::all, ff::size_pick::current, ff::sector_pick::fresh});
			CHECK(snapshot_state(fs) == before);
		}
	});
	return faultlab_test::verdict();
}
