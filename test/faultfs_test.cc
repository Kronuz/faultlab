// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs's additions over strata's model: directories (F4), kill, snapshot and powered_off (F6),
// and the F7 members: link, try_lease, map_shared, preallocate, inspection and owner locks, with
// the crash rules the design's build steps list for link, try_lease and map_shared.
#include "faultlab/faultfs.h"

#include "check.h"

#include <set>

namespace ff = kronuz::faultlab;
namespace j = kronuz::journal;
using names = std::vector<std::string>;

namespace {

std::string bytes(std::size_t n, char c) { return std::string(n, c); }

std::string read_all(j::File& f) {
	std::string out(f.size(), '\0');
	f.read_at(0, out);
	return out;
}

}  // namespace

int main() {
	using faultlab_test::run;

	// F4: one device, a directory per io.
	run("directories: entries of their parent, durable at once", [] {
		ff::filesystem fs;
		fs.make_directory("d");
		fs.make_directory("d/e");
		CHECK_ERRNO(fs.make_directory("x/y"), ENOENT);
		CHECK_ERRNO(fs.make_directory("d"), EEXIST);
		CHECK_ERRNO(ff::io(fs, "missing"), ENOENT);
		ff::io root(fs), d(fs, "d");
		root.create_exclusive("f");
		d.create_exclusive("g");
		CHECK(fs.list() == (names{"d", "f"}));
		CHECK(fs.list("d") == (names{"e", "g"}));
		auto cursor = root.scan_directory();
		names scanned;
		while (auto n = cursor->next()) { scanned.push_back(*n); }
		CHECK(scanned == (names{"d", "f"}));
		CHECK(root.entry_footprint("d")->kind == j::EntryKind::Directory);
		CHECK(root.inspect_entry("d")->footprint.kind == j::EntryKind::Directory);
		CHECK_ERRNO(root.remove("d"), EPERM);
		CHECK_ERRNO(root.open_existing("d"), EISDIR);
		CHECK_ERRNO(root.create_exclusive("d"), EEXIST);
		fs.crash(1, {.names = ff::name_pick::durable});
		CHECK(fs.list() == names{"d"});  // the directories survive; the unsynced files do not
		CHECK(fs.list("d") == names{"e"});
	});
	run("sync_directory makes only its directory durable", [] {
		for (std::uint64_t seed = 0; seed < 16; ++seed) {
			ff::filesystem fs;
			fs.make_directory("d");
			ff::io root(fs), d(fs, "d");
			root.create_exclusive("f");
			d.create_exclusive("g");
			d.sync_directory();
			fs.crash(seed, {.names = ff::name_pick::durable});
			CHECK(fs.list() == names{"d"});
			CHECK(fs.list("d") == names{"g"});
		}
	});
	run("an ordering point orders every directory of the device", [] {
		std::set<names> seen;
		for (std::uint64_t seed = 0; seed < 256; ++seed) {
			ff::filesystem fs;
			fs.make_directory("d");
			ff::io root(fs), d(fs, "d");
			d.create_exclusive("x");
			root.order_files_and_directory({});
			root.create_exclusive("y");
			fs.crash(seed);
			const auto top = fs.list(), sub = fs.list("d");
			CHECK(std::count(top.begin(), top.end(), "y") == 0 || sub == names{"x"});  // y never without x
			seen.insert(top);
		}
		CHECK(seen.count(names{"d", "y"}));
	});

	// F6.
	run("kill: handles go stale, the bytes and the epochs stay", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		io.sync_directory();
		f->write_at(0, bytes(1024, 'x'));
		j::File* files[] = {f.get()};
		io.order_files_and_directory(files);
		f->write_at(1024, bytes(512, 'y'));
		auto owner = io.acquire_owner(j::OwnerLockOptions{j::OwnerLockMode::both, j::OwnerFile::create_exclusive});
		fs.kill();
		CHECK_ERRNO(f->size(), EIO);
		CHECK(fs.view("f") == bytes(1024, 'x') + bytes(512, 'y'));
		CHECK(io.acquire_owner(j::OwnerLockOptions{}) != nullptr);  // released by the kill
		auto again = fs.snapshot();
		fs.crash(3, {.epochs = ff::epoch_pick::none, .size = ff::size_pick::current, .sectors = ff::sector_pick::fresh});
		CHECK(fs.view("f") == bytes(1024, 'x'));  // the staged epoch outlived the kill
		again.crash(3, {.epochs = ff::epoch_pick::all, .size = ff::size_pick::current, .sectors = ff::sector_pick::fresh});
		CHECK(again.view("f") == bytes(1024, 'x') + bytes(512, 'y'));
	});
	run("snapshot: a deep copy, independent of the original", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		f->write_at(0, "abc");
		auto copy = fs.snapshot();
		f->write_at(0, "xyz");
		io.create_exclusive("g");
		CHECK(copy.view("f") == "abc");
		CHECK(copy.list() == names{"f"});
		CHECK(fs.view("f") == "xyz");
		CHECK(copy.mutations() == 2);
		ff::io other(copy);
		auto h = other.open_existing("f");
		h->write_at(3, "d");
		CHECK(copy.view("f") == "abcd");
		CHECK(fs.view("f") == "xyz");
	});
	run("powered_off: true once the power runs out, until crash", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		fs.power_off_after(1);
		CHECK(!fs.powered_off());
		f->write_at(0, "a");
		CHECK(fs.powered_off());
		CHECK_ERRNO(f->write_at(1, "b"), EIO);
		fs.crash(0);
		CHECK(!fs.powered_off());
	});

	// F7 members.
	run("preallocate: grows, reserves, and fails before its effect", [] {
		ff::filesystem fs(4096);
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		const auto before = fs.mutations();
		f->preallocate(0, 3000);
		CHECK(fs.mutations() == before + 1);
		CHECK(f->size() == 3000);
		CHECK(fs.used() == 3000);
		f->truncate(1000);
		CHECK(fs.used() == 1000);
		f->preallocate(500, 2500);
		f->write_at(0, bytes(3000, 'w'));  // inside the reservation
		CHECK_ERRNO(f->preallocate(0, 5000), ENOSPC);
		CHECK(f->size() == 3000);
		fs.add_fault({ff::op::preallocate, 1, EIO});
		CHECK_ERRNO(f->preallocate(0, 3500), EIO);
		CHECK(f->size() == 3000);
		fs.power_off_after(0);
		CHECK_ERRNO(f->preallocate(0, 3500), EIO);
		CHECK(f->size() == 3000);
	});
	run("inspection: kind, size, inode, links and mode", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("a");
		f->write_at(0, "12345");
		io.link("a", "b");
		const auto held = f->inspect();
		CHECK(held.footprint.kind == j::EntryKind::Regular);
		CHECK(held.footprint.logical_bytes == 5);
		CHECK(held.links == 2);
		CHECK(held.permissions == 0600);
		CHECK(held.owner == ::geteuid());
		CHECK(!held.extended_acl);
		CHECK(io.inspect_entry("b")->inode == held.inode);
		CHECK(io.inspect_entry("a") == io.inspect_entry("b"));
		CHECK(!io.inspect_entry("c"));
		io.remove("a");
		CHECK(io.inspect_entry("b")->links == 1);
	});
	run("owner locks: file, directory and both", [] {
		ff::filesystem fs;
		ff::io io(fs);
		using M = j::OwnerLockMode;
		using F = j::OwnerFile;
		CHECK_ERRNO(io.acquire_owner(j::OwnerLockOptions{M::file, F::open_existing}), ENOENT);
		auto file = io.acquire_owner(j::OwnerLockOptions{M::file, F::create_exclusive});
		auto dir = io.acquire_owner(j::OwnerLockOptions{M::directory, F::open_existing});  // a different inode
		CHECK_ERRNO(io.acquire_owner(j::OwnerLockOptions{M::both, F::open_existing}), EWOULDBLOCK);
		CHECK_ERRNO(io.acquire_owner(j::OwnerLockOptions{M::file, F::open_existing}), EWOULDBLOCK);
		CHECK_ERRNO(io.acquire_directory_owner(), EWOULDBLOCK);
		file.reset();
		CHECK_ERRNO(io.acquire_owner(true), EEXIST);
		CHECK(io.acquire_owner(false) != nullptr);
		dir.reset();
		auto both = io.acquire_owner(j::OwnerLockOptions{M::both, F::open_existing});
		CHECK_ERRNO(io.acquire_owner(j::OwnerLockOptions{M::directory, F::open_existing}), EWOULDBLOCK);
		fs.make_directory("other");
		ff::io other(fs, "other");
		CHECK(other.acquire_owner(j::OwnerLockOptions{M::directory, F::open_existing}) != nullptr);  // per directory
		io.sync_directory();
		fs.crash(0);
		CHECK(io.acquire_owner(j::OwnerLockOptions{M::both, F::open_existing}) != nullptr);
	});

	// A4: link.
	run("link: a second name, never a replacement", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("a");
		f->write_at(0, "data");
		io.create_exclusive("c");
		CHECK_ERRNO(io.link("missing", "b"), ENOENT);
		CHECK_ERRNO(io.link("a", "c"), EEXIST);
		const auto before = fs.mutations();
		io.link("a", "b");
		CHECK(fs.mutations() == before + 1);
		CHECK(fs.view("b") == "data");
		f->write_at(0, "DATA");
		f->sync();
		CHECK(fs.view("b") == "DATA");
		fs.add_fault({ff::op::link, 1, EIO});
		CHECK_ERRNO(io.link("a", "d"), EIO);
		CHECK(fs.list() == (names{"a", "b", "c"}));
		io.sync_directory();
		fs.crash(0);
		CHECK(fs.list() == (names{"a", "b", "c"}));
		CHECK(fs.view("b") == "DATA");
	});
	run("link then unlink of the temporary keeps only POSIX-reachable states", [] {
		for (bool synced_temporary : {true, false}) {
			std::set<names> seen;
			for (std::uint64_t seed = 0; seed < 512; ++seed) {
				ff::filesystem fs;
				ff::io io(fs);
				auto f = io.create_exclusive("tmp");
				f->write_at(0, "published");
				f->sync();
				if (synced_temporary) { io.sync_directory(); }
				io.link("tmp", "final");
				io.remove("tmp");
				fs.crash(seed);
				const auto state = fs.list();
				seen.insert(state);
				const std::set<names> legal = synced_temporary ? std::set<names>{{"tmp"}, {"final", "tmp"}, {"final"}}
															   : std::set<names>{{}, {"tmp"}, {"final", "tmp"}, {"final"}};
				if (!CHECK(legal.count(state))) { std::fprintf(stderr, "  seed %llu leaves %zu names\n", (unsigned long long)seed, state.size()); }
				for (const auto& n : state) { CHECK(fs.view(n) == "published"); }
			}
			CHECK(seen.count(names{"tmp"}) && seen.count(names{"final", "tmp"}) && seen.count(names{"final"}));
		}
	});
	run("a replace over a linked name cannot outlive its link", [] {
		for (std::uint64_t seed = 0; seed < 512; ++seed) {
			ff::filesystem fs;
			ff::io io(fs);
			auto a = io.create_exclusive("a");
			a->write_at(0, "old");
			a->sync();
			auto n = io.create_exclusive("new");
			n->write_at(0, "new");
			n->sync();
			io.sync_directory();
			io.link("a", "keep");
			io.replace("new", "a");
			fs.crash(seed);
			const auto state = fs.list();
			// The old file stays named: by "a" or "keep".
			bool old_named = false;
			for (const auto& name : state) { old_named = old_named || fs.view(name) == "old"; }
			CHECK(old_named);
		}
	});

	// A4: try_lease.
	run("try_lease: shared leases coexist, an exclusive one excludes", [] {
		ff::filesystem fs;
		ff::io io(fs);
		io.create_exclusive("f");
		auto a = io.open_existing("f"), b = io.open_existing("f"), c = io.open_existing("f");
		CHECK(a->try_lease(j::LeaseMode::Shared));
		CHECK(b->try_lease(j::LeaseMode::Shared));
		CHECK(!c->try_lease(j::LeaseMode::Exclusive));
		a->close();
		CHECK(!c->try_lease(j::LeaseMode::Exclusive));
		b.reset();  // destroying the handle releases its lease
		CHECK(c->try_lease(j::LeaseMode::Exclusive));
		auto d = io.open_existing("f");
		CHECK(!d->try_lease(j::LeaseMode::Shared));
		bool threw = false;
		try {
			c->try_lease(j::LeaseMode::Shared);
		} catch (const std::logic_error&) {
			threw = true;
		}
		CHECK(threw);  // fresh handles only
		fs.add_fault({ff::op::lease, 1, EIO});
		const auto before = fs.mutations();
		CHECK_ERRNO(d->try_lease(j::LeaseMode::Shared), EIO);
		CHECK(fs.mutations() == before);  // a lease is no mutation
	});
	run("a lease held at a cut is released by kill and by crash", [] {
		for (bool by_kill : {true, false}) {
			ff::filesystem fs;
			ff::io io(fs);
			io.create_exclusive("f");
			io.sync_directory();
			auto holder = io.open_existing("f");
			CHECK(holder->try_lease(j::LeaseMode::Exclusive));
			fs.power_off_after(0);  // the cut
			auto other = io.open_existing("f");
			CHECK(!other->try_lease(j::LeaseMode::Exclusive));
			if (by_kill) {
				fs.kill();
			} else {
				fs.crash(0);
			}
			auto next = io.open_existing("f");
			CHECK(next->try_lease(j::LeaseMode::Exclusive));
			holder.reset();  // a stale holder's release leaves the new lease alone
			auto late = io.open_existing("f");
			CHECK(!late->try_lease(j::LeaseMode::Shared));
		}
	});

	// A4: map_shared.
	run("map_shared: stores reach the file at the next call, not past the end", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		f->write_at(0, bytes(2048, 'a'));
		CHECK_ERRNO(f->map_shared(4096), EINVAL);
		auto ro = io.open_existing_read_only("f");
		CHECK_ERRNO(ro->map_shared(1024), EACCES);
		auto m = f->map_shared(1024);
		auto words = m->words();
		CHECK(words.size() == 1024);
		words[10] = 'X';
		CHECK(read_all(*f)[10] == 'X');
		f->write_at(20, "W");
		CHECK(m->words()[20] == 'W');  // writes reach the mapping
		words[30] = 'Y';
		m.reset();  // unmapping keeps the store
		CHECK(fs.view("f")[30] == 'Y');
	});
	run("map_shared: a fold moves no mutation count", [] {
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		f->write_at(0, bytes(4096, 'a'));
		auto m = f->map_shared(4096);
		const auto mutations = fs.mutations();
		fs.power_off_after(1);
		for (int i = 0; i < 8; ++i) {
			m->words()[std::size_t(i) * 512] = 'Z';
			f->size();
		}
		CHECK(fs.mutations() == mutations);
		CHECK(!fs.powered_off());
		CHECK(fs.view("f")[3584] == 'Z');
	});
	run("map_shared: stores tear per sector and never pass an ordering point they followed", [] {
		const std::string d = bytes(2048, 'd');
		std::set<std::string> seen;
		bool torn = false;
		for (std::uint64_t seed = 0; seed < 512; ++seed) {
			ff::filesystem fs;
			ff::io io(fs);
			auto f = io.create_exclusive("f");
			f->write_at(0, d);
			f->sync();
			io.sync_directory();
			auto m = f->map_shared(2048);
			auto w = m->words();
			for (std::size_t s : {0, 1, 2, 3}) { w[s * 512] = '1'; }  // before the ordering point
			j::File* files[] = {f.get()};
			io.order_files_and_directory(files);
			for (std::size_t s : {0, 1, 2, 3}) { w[s * 512 + 1] = '2'; }  // after it
			fs.crash(seed);
			const auto state = std::string(fs.view("f"));
			seen.insert(state);
			int ones = 0, twos = 0;
			for (std::size_t s = 0; s < 4; ++s) {
				ones += state[s * 512] == '1';
				twos += state[s * 512 + 1] == '2';
			}
			CHECK(twos == 0 || ones == 4);  // a later store never survives without the epoch
			torn = torn || (ones > 0 && ones < 4);
			m.reset();
		}
		CHECK(torn);
		CHECK(seen.size() > 8);
	});
	run("map_shared: stores before power off can survive a crash; a kill keeps every word", [] {
		bool kept = false;
		for (std::uint64_t seed = 0; seed < 64; ++seed) {
			ff::filesystem fs;
			ff::io io(fs);
			auto f = io.create_exclusive("f");
			f->write_at(0, bytes(1024, 'a'));
			f->sync();
			io.sync_directory();
			auto m = f->map_shared(1024);
			m->words()[700] = 'S';
			fs.power_off_after(0);
			fs.crash(seed);
			kept = kept || fs.view("f")[700] == 'S';
			m->words()[701] = 'T';  // detached: goes nowhere
			m.reset();
			CHECK(fs.view("f")[701] == 'a');
		}
		CHECK(kept);
		ff::filesystem fs;
		ff::io io(fs);
		auto f = io.create_exclusive("f");
		f->write_at(0, bytes(1024, 'a'));
		auto m = f->map_shared(1024);
		for (std::size_t i = 0; i < 1024; i += 100) { m->words()[i] = 'K'; }
		const std::string stored(m->words().data(), 1024);
		fs.kill();
		CHECK(fs.view("f") == stored);
		m->words()[1] = 'Q';
		CHECK(fs.view("f") == stored);
	});
	return faultlab_test::verdict();
}
