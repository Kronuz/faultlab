// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The recorder over completion's PosixIO: events per primitive, cuts recovered on faultfs and on
// disk, faults on the real calls, and verify() failing on changes made outside the seam.
#include "faultlab/recorder.h"

#include "completion/posix.h"

#include "check.h"

#include <unistd.h>

namespace ff = kronuz::faultlab;
namespace j = kronuz::journal;
namespace fs = std::filesystem;
using names = std::vector<std::string>;

namespace {

// A fresh private directory under the build's .scratch.
fs::path fresh(const std::string& name) {
	const fs::path dir = fs::path(".scratch") / ("recorder-" + std::to_string(::getpid())) / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
	return dir;
}

bool verifies(const ff::recorder& rec, const fs::path& dir, std::span<const std::string_view> length_only = {}) {
	try {
		rec.verify(dir, length_only);
		return true;
	} catch (const std::runtime_error& e) {
		std::printf("    verify: %s\n", e.what());
		return false;
	}
}

std::vector<std::string> labels(const ff::recorder& rec) {
	std::vector<std::string> out;
	for (const auto& e : rec.events()) { out.push_back(e.label); }
	return out;
}

}  // namespace

int main() {
	using faultlab_test::run;

	run("every mutating primitive is one event; the shadow matches the real directory", [] {
		const auto dir = fresh("events");
		auto shadow = ff::load(dir);
		ff::recorder rec(std::make_shared<j::PosixIO>(dir), shadow);
		auto f = rec.create_exclusive("tmp");
		f->write_at(0, "published");
		f->sync();
		rec.link("tmp", "final");
		rec.remove("tmp");
		rec.sync_directory();
		rec.note("published");
		auto g = rec.create_exclusive("g");
		g->write_at(0, "abc");
		j::File* files[] = {f.get(), g.get()};
		rec.sync_files_and_directory(files);
		CHECK(labels(rec) == (names{"start", "create tmp", "write tmp", "sync tmp", "link tmp final", "remove tmp", "sync_directory", "published", "create g", "write g", "sync tmp", "sync g", "sync_directory"}));
		CHECK(verifies(rec, dir));
		CHECK(rec.events()[4].state.list() == (names{"final", "tmp"}));
	});
	run("a cut after an event recovers on faultfs and on disk", [] {
		const auto dir = fresh("cut");
		auto shadow = ff::load(dir);
		ff::recorder rec(std::make_shared<j::PosixIO>(dir), shadow);
		auto f = rec.create_exclusive("data");
		f->write_at(0, std::string(1500, 'x'));
		f->sync();
		rec.sync_directory();
		f->write_at(1500, std::string(600, 'y'));
		std::set<std::uint64_t> sizes;
		const auto& cut = rec.events().back();
		CHECK(cut.label == "write data");
		for (std::uint64_t seed = 0; seed < 64; ++seed) {
			auto state = cut.state.snapshot();
			state.crash(seed);
			sizes.insert(state.view("data").size());
		}
		CHECK(sizes == (std::set<std::uint64_t>{1500, 2100}));
		auto state = cut.state.snapshot();
		state.crash(1, {.size = ff::size_pick::durable});
		const auto out = fresh("cut-materialized");
		ff::materialize(state, out);
		j::PosixIO recovered(out);
		auto r = recovered.open_existing("data");
		CHECK(r->size() == 1500);
		auto back = ff::load(out);
		CHECK(back.view("data") == state.view("data"));
	});
	run("faults fail the real call before or after it", [] {
		const auto dir = fresh("faults");
		auto shadow = ff::load(dir);
		ff::recorder rec(std::make_shared<j::PosixIO>(dir), shadow);
		auto f = rec.create_exclusive("f");
		rec.add_fault({ff::op::write, 1, ENOSPC});
		CHECK_ERRNO(f->write_at(0, "lost"), ENOSPC);
		CHECK(f->size() == 0);
		rec.add_fault({ff::op::sync, 1, EIO, true});
		f->write_at(0, "kept");
		const auto before = rec.events().size();
		CHECK_ERRNO(f->sync(), EIO);
		CHECK(rec.events().size() == before + 1);  // it happened, so it is recorded
		CHECK(verifies(rec, dir));
	});
	run("verify catches a raw write and a raw link", [] {
		const auto dir = fresh("raw");
		auto shadow = ff::load(dir);
		ff::recorder rec(std::make_shared<j::PosixIO>(dir), shadow);
		auto f = rec.create_exclusive("f");
		f->write_at(0, "through the seam");
		CHECK(verifies(rec, dir));
		{
			std::fstream raw(dir / "f", std::ios::in | std::ios::out | std::ios::binary);
			raw.seekp(0);
			raw.write("THROUGH", 7);
		}
		CHECK(!verifies(rec, dir));  // same length, other bytes
		f->write_at(0, "through");
		CHECK(verifies(rec, dir));
		fs::create_hard_link(dir / "f", dir / "raw-link");
		CHECK(!verifies(rec, dir));
	});
	run("a mapped file verifies by length only", [] {
		const auto dir = fresh("mapped");
		auto shadow = ff::load(dir);
		ff::recorder rec(std::make_shared<j::PosixIO>(dir), shadow);
		auto f = rec.create_exclusive("change");
		f->truncate(4096);
		auto m = f->map_shared(4096);
		if (!m) {
			std::printf("    map_shared unsupported here\n");
			return;
		}
		m->words()[100] = 'M';
		f->sync();
		CHECK(!verifies(rec, dir));
		const std::string_view length_only[] = {"change"};
		CHECK(verifies(rec, dir, length_only));
	});
	run("the shadow loads a real directory with its subdirectories", [] {
		const auto dir = fresh("load");
		fs::create_directory(dir / "sub");
		std::ofstream(dir / "a", std::ios::binary) << "alpha";
		std::ofstream(dir / "sub" / "b", std::ios::binary) << "beta";
		auto shadow = ff::load(dir);
		CHECK(shadow.list() == (names{"a", "sub"}));
		CHECK(shadow.view("sub/b") == "beta");
		shadow.crash(0);  // durable
		CHECK(shadow.view("a") == "alpha");
		ff::recorder rec(std::make_shared<j::PosixIO>(dir / "sub"), shadow, "sub");
		CHECK(verifies(rec, dir / "sub"));
	});
	fs::remove_all(fs::path(".scratch") / ("recorder-" + std::to_string(::getpid())));
	return faultlab_test::verdict();
}
