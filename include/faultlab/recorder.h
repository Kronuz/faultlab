// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// recorder: a journal::IO decorator that runs every call on a real IO and then on a shadow
// faultfs, and after each mutating primitive records {label, shadow.snapshot()}. A cut after
// event i is events()[i].state.snapshot() crashed with a seed and a policy, or killed; recovery
// then runs on that faultfs, or on a real directory through materialize(). A group sync reaches
// the real IO once, keeping its shared flush, and the shadow as its primitives, one event each.
// Mapped stores (map_shared) reach the real file only, so the shadow holds a mapped file's name
// and length, not its words. A run ends with verify(): a change made outside the seam (a raw
// link, a raw write) leaves the shadow behind the real directory and fails there.
#pragma once

#include "faultlab/faultfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kronuz::faultlab {

namespace detail {

inline std::string read_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) { fail(EIO, "faultlab: read"); }
	return std::string(std::istreambuf_iterator<char>(in), {});
}

// A real directory's files and subdirectories, by path relative to it.
inline void scan_real(const std::filesystem::path& root, const std::filesystem::path& at, std::map<std::string, std::string>& files, std::set<std::string>& directories) {
	for (const auto& entry : std::filesystem::directory_iterator(at)) {
		const std::string rel = std::filesystem::relative(entry.path(), root).generic_string();
		const auto status = entry.symlink_status();
		if (std::filesystem::is_directory(status)) {
			directories.insert(rel);
			scan_real(root, entry.path(), files, directories);
		} else if (std::filesystem::is_regular_file(status)) {
			files[rel] = read_file(entry.path());
		} else {
			files[rel] = "\0<not a regular file>";
		}
	}
}

}  // namespace detail

// A durable model of a real directory: its subdirectories, files and bytes, all synced.
inline filesystem load(const std::filesystem::path& directory) {
	std::map<std::string, std::string> files;
	std::set<std::string> directories;
	detail::scan_real(directory, directory, files, directories);
	filesystem fs;
	for (const auto& d : directories) { fs.make_directory(d); }  // sorted: parents first
	for (const auto& [path, bytes] : files) {
		io dir(fs, detail::parent(path));
		auto f = dir.create_exclusive(detail::base(path));
		f->write_at(0, bytes);
		f->sync();
	}
	io(fs).sync_directory();
	for (const auto& d : directories) { io(fs, d).sync_directory(); }
	return fs;
}

// A model's current names and bytes on disk, in `directory`, which must be missing or empty.
// The directories are private and the files owner-only, as PosixIO requires.
inline void materialize(const filesystem& fs, const std::filesystem::path& directory) {
	namespace stdfs = std::filesystem;
	stdfs::create_directories(directory);
	if (!stdfs::is_empty(directory)) { throw std::invalid_argument("faultlab materialize: the directory is not empty"); }
	stdfs::permissions(directory, stdfs::perms::owner_all, stdfs::perm_options::replace);
	for (const auto& d : detail::model_access::directories(fs)) {
		stdfs::create_directory(directory / d);
		stdfs::permissions(directory / d, stdfs::perms::owner_all, stdfs::perm_options::replace);
	}
	for (const auto& [path, bytes] : detail::model_access::files(fs)) {
		{
			std::ofstream out(directory / path, std::ios::binary | std::ios::trunc);
			out.write(bytes.data(), std::streamsize(bytes.size()));
			if (!out) { detail::fail(EIO, "faultlab materialize: write"); }
		}
		stdfs::permissions(directory / path, stdfs::perms::owner_read | stdfs::perms::owner_write, stdfs::perm_options::replace);
	}
}

class recorder final : public journal::IO {
public:
	struct event {
		std::string label;
		filesystem state;
	};

	// The shadow is the directory `directory` of `shadow`, as an io is; event 0 is its start.
	recorder(std::shared_ptr<journal::IO> real, filesystem& shadow, std::string directory = {})
		: real_(std::move(real)), shadow_fs_(&shadow), shadow_(shadow, directory), directory_(std::move(directory)) {
		events_.push_back({"start", shadow_fs_->snapshot()});
	}

	// A labelled event, e.g. from a test hook at a code point.
	void note(std::string label) {
		std::lock_guard lock(mu_);
		events_.push_back({std::move(label), shadow_fs_->snapshot()});
	}
	// Fails the `nth` real call of `kind` before it (nothing happens) or after it (both happen
	// and the event is recorded), with `error`, counting as faultfs rules do.
	void add_fault(const fault& f) {
		std::lock_guard lock(mu_);
		rules_.push_back({f, 0});
	}
	// Throws unless the shadow's current names and bytes equal the real directory's; the files in
	// `length_only` (mapped through map_shared, whose stores the shadow never sees) by length only.
	void verify(const std::filesystem::path& real_directory, std::span<const std::string_view> length_only = {}) const {
		std::map<std::string, std::string> real_files;
		std::set<std::string> real_directories;
		detail::scan_real(real_directory, real_directory, real_files, real_directories);
		std::map<std::string, std::string> shadow_files;
		std::set<std::string> shadow_directories;
		const std::string prefix = directory_.empty() ? std::string() : directory_ + "/";
		for (auto& [path, bytes] : detail::model_access::files(*shadow_fs_)) {
			if (path.starts_with(prefix)) { shadow_files[path.substr(prefix.size())] = std::move(bytes); }
		}
		for (const auto& d : detail::model_access::directories(*shadow_fs_)) {
			if (d.starts_with(prefix) && d.size() > prefix.size()) { shadow_directories.insert(d.substr(prefix.size())); }
		}
		std::ostringstream diff;
		for (const auto& d : real_directories) {
			if (!shadow_directories.count(d)) { diff << "\n  directory " << d << " only in the real directory"; }
		}
		for (const auto& d : shadow_directories) {
			if (!real_directories.count(d)) { diff << "\n  directory " << d << " only in the shadow"; }
		}
		for (const auto& [path, bytes] : real_files) {
			auto it = shadow_files.find(path);
			if (it == shadow_files.end()) {
				diff << "\n  " << path << " only in the real directory";
			} else if (it->second.size() != bytes.size()) {
				diff << "\n  " << path << ": " << bytes.size() << " bytes, the shadow " << it->second.size();
			} else if (it->second != bytes && std::find(length_only.begin(), length_only.end(), path) == length_only.end()) {
				diff << "\n  " << path << ": the bytes differ from the shadow's";
			}
		}
		for (const auto& [path, bytes] : shadow_files) {
			if (!real_files.count(path)) { diff << "\n  " << path << " only in the shadow"; }
		}
		if (!diff.str().empty()) { throw std::runtime_error("faultlab recorder: the shadow does not match " + real_directory.string() + ":" + diff.str()); }
	}
	// The recorded events; read them once the run's calls are done.
	const std::vector<event>& events() const { return events_; }

	journal::FilesystemClass directory_filesystem() const override { return real_->directory_filesystem(); }
	journal::DirectoryTrust directory_trust() const override { return real_->directory_trust(); }
	using journal::IO::acquire_owner;
	std::unique_ptr<journal::OwnerLock> acquire_owner(bool create) override {
		return own("owner", create, [&] { return real_->acquire_owner(create); }, [&] { return shadow_.acquire_owner(create); });
	}
	std::unique_ptr<journal::OwnerLock> acquire_directory_owner() override {
		return own("directory owner", false, [&] { return real_->acquire_directory_owner(); }, [&] { return shadow_.acquire_directory_owner(); });
	}
	std::unique_ptr<journal::File> open_existing(std::string_view name) override {
		return opened(name, real_->open_existing(name), [&] { return shadow_.open_existing(name); });
	}
	std::unique_ptr<journal::File> open_existing_read_only(std::string_view name) override {
		return opened(name, real_->open_existing_read_only(name), [&] { return shadow_.open_existing_read_only(name); });
	}
	std::unique_ptr<journal::File> open_reclaim_candidate(std::string_view name) override {
		return opened(name, real_->open_reclaim_candidate(name), [&] { return shadow_.open_existing(name); });
	}
	std::unique_ptr<journal::File> open_reclaim_candidate_read_only(std::string_view name) override {
		return opened(name, real_->open_reclaim_candidate_read_only(name), [&] { return shadow_.open_existing_read_only(name); });
	}
	void open_existing_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable file open unsupported"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable candidate open unsupported"); }
	std::unique_ptr<journal::File> create_exclusive(std::string_view name) override {
		std::unique_ptr<journal::File> real, shadow;
		primitive(op::create, "create " + std::string(name), [&] { real = real_->create_exclusive(name); }, [&] { shadow = shadow_.create_exclusive(name); });
		return std::make_unique<file>(*this, std::string(name), std::move(real), std::move(shadow));
	}
	void replace(std::string_view source, std::string_view destination) override {
		primitive(op::replace, "replace " + std::string(source) + " " + std::string(destination), [&] { real_->replace(source, destination); }, [&] { shadow_.replace(source, destination); });
	}
	void link(std::string_view source, std::string_view destination) override {
		primitive(op::link, "link " + std::string(source) + " " + std::string(destination), [&] { real_->link(source, destination); }, [&] { shadow_.link(source, destination); });
	}
	void remove(std::string_view name) override {
		primitive(op::remove, "remove " + std::string(name), [&] { real_->remove(name); }, [&] { shadow_.remove(name); });
	}
	std::unique_ptr<journal::DirectoryCursor> scan_directory() override { return real_->scan_directory(); }
	std::optional<journal::EntryFootprint> entry_footprint(std::string_view name) override { return real_->entry_footprint(name); }
	std::optional<journal::EntryInspection> inspect_entry(std::string_view name) override { return real_->inspect_entry(name); }
	void sync_directory() override {
		primitive(op::sync_directory, "sync_directory", [&] { real_->sync_directory(); }, [&] { shadow_.sync_directory(); });
	}
	void sync_files(std::span<journal::File* const> files) override { group(files, false, false); }
	void sync_data_files(std::span<journal::File* const> files) override { group(files, true, false); }
	void sync_files_and_directory(std::span<journal::File* const> files) override { group(files, false, true); }
	void order_files_and_directory(std::span<journal::File* const> files) override {
		auto [reals, shadows] = split(files);
		primitive(op::sync_directory, "order_files_and_directory", [&] { real_->order_files_and_directory(reals); }, [&] { shadow_.order_files_and_directory(shadows); });
	}

protected:
	std::unique_ptr<journal::OwnerLock> lock_owner(const journal::OwnerLockOptions& options) override {
		const bool create = options.owner_file == journal::OwnerFile::create_exclusive;
		return own("owner", create, [&] { return real_->acquire_owner(options); }, [&] { return shadow_.acquire_owner(options); });
	}

private:
	class file final : public journal::File {
	public:
		file(recorder& r, std::string name, std::unique_ptr<journal::File> real, std::unique_ptr<journal::File> shadow)
			: rec_(&r), name_(std::move(name)), real_(std::move(real)), shadow_(std::move(shadow)) {}

		std::uint64_t size() override { return real_->size(); }
		std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override { return real_->read_at(offset, bytes); }
		std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
			std::size_t n = 0;
			rec_->primitive(op::write, "write " + name_, [&] { n = real_->write_at(offset, bytes); }, [&] {
				for (std::size_t at = 0; at < n;) { at += shadow_->write_at(offset + at, bytes.substr(at, n - at)); }
			});
			return n;
		}
		void truncate(std::uint64_t size) override {
			rec_->primitive(op::truncate, "truncate " + name_, [&] { real_->truncate(size); }, [&] { shadow_->truncate(size); });
		}
		void sync() override {
			rec_->primitive(op::sync, "sync " + name_, [&] { real_->sync(); }, [&] { shadow_->sync(); });
		}
		void sync_data() override {
			rec_->primitive(op::sync, "sync_data " + name_, [&] { real_->sync_data(); }, [&] { shadow_->sync_data(); });
		}
		void preallocate(std::uint64_t offset, std::uint64_t length) override {
			rec_->primitive(op::preallocate, "preallocate " + name_, [&] { real_->preallocate(offset, length); }, [&] { shadow_->preallocate(offset, length); });
		}
		void advise(std::uint64_t offset, std::uint64_t length, journal::Advice advice) noexcept override { real_->advise(offset, length, advice); }
		journal::EntryInspection inspect() override { return real_->inspect(); }
		bool try_lease(journal::LeaseMode mode) override { return real_->try_lease(mode); }
		void close() override {
			real_->close();
			shadow_->close();
		}
		std::shared_ptr<const journal::Mapping> map() override { return real_->map(); }
		std::shared_ptr<journal::SharedMapping> map_shared(std::uint64_t length) override { return real_->map_shared(length); }

		recorder* rec_;
		std::string name_;
		std::unique_ptr<journal::File> real_, shadow_;
	};
	struct pair_lock final : journal::OwnerLock {
		std::unique_ptr<journal::OwnerLock> real, shadow;
	};
	struct rule {
		fault f;
		std::uint64_t seen;
	};

	const fault* fire(op kind) {
		for (auto& r : rules_) {
			if (r.f.kind == kind && ++r.seen == r.f.nth) { return &r.f; }
		}
		return nullptr;
	}
	// The real call, then the shadow's, then the event; a shadow that refuses what the real IO did
	// has diverged from it, which no later state could explain.
	template <class Real, class Shadow>
	void primitive(op kind, std::string label, Real&& real, Shadow&& shadow) {
		std::lock_guard lock(mu_);
		const fault* f = fire(kind);
		if (f && !f->after) { detail::fail(f->error, "faultlab recorder"); }
		real();
		apply_shadow(label, shadow);
		events_.push_back({std::move(label), shadow_fs_->snapshot()});
		if (f) { detail::fail(f->error, "faultlab recorder"); }
	}
	template <class Shadow>
	static void apply_shadow(const std::string& label, Shadow&& shadow) {
		try {
			shadow();
		} catch (const std::exception& e) {
			throw std::logic_error("faultlab recorder: the shadow diverged at " + label + ": " + e.what());
		}
	}
	template <class Real, class Shadow>
	std::unique_ptr<journal::OwnerLock> own(const char* label, bool create, Real&& real, Shadow&& shadow) {
		auto out = std::make_unique<pair_lock>();
		std::lock_guard lock(mu_);
		out->real = real();
		apply_shadow(label, [&] { out->shadow = shadow(); });
		if (create) { events_.push_back({label, shadow_fs_->snapshot()}); }
		return out;
	}
	template <class Shadow>
	std::unique_ptr<journal::File> opened(std::string_view name, std::unique_ptr<journal::File> real, Shadow&& shadow) {
		if (!real) { return nullptr; }
		std::unique_ptr<journal::File> s;
		apply_shadow("open " + std::string(name), [&] { s = shadow(); });
		return std::make_unique<file>(*this, std::string(name), std::move(real), std::move(s));
	}
	std::pair<std::vector<journal::File*>, std::vector<journal::File*>> split(std::span<journal::File* const> files) {
		std::pair<std::vector<journal::File*>, std::vector<journal::File*>> out;
		for (auto* f : files) {
			auto* own = dynamic_cast<file*>(f);
			if (!own || own->rec_ != this) { throw std::invalid_argument("faultlab recorder: a file this recorder did not open"); }
			out.first.push_back(own->real_.get());
			out.second.push_back(own->shadow_.get());
		}
		return out;
	}
	// One real group call; on the shadow, each file's sync (and the directory's) with an event each.
	void group(std::span<journal::File* const> files, bool data, bool directory) {
		auto [reals, shadows] = split(files);
		std::lock_guard lock(mu_);
		const fault* f = nullptr;
		for (std::size_t i = 0; i < files.size() && !f; ++i) { f = fire(op::sync); }
		if (!f && directory) { f = fire(op::sync_directory); }
		if (f && !f->after) { detail::fail(f->error, "faultlab recorder"); }
		if (directory) {
			real_->sync_files_and_directory(reals);
		} else if (data) {
			real_->sync_data_files(reals);
		} else {
			real_->sync_files(reals);
		}
		for (std::size_t i = 0; i < shadows.size(); ++i) {
			const std::string label = (data ? "sync_data " : "sync ") + static_cast<file*>(files[i])->name_;
			apply_shadow(label, [&] { data ? shadows[i]->sync_data() : shadows[i]->sync(); });
			events_.push_back({label, shadow_fs_->snapshot()});
		}
		if (directory) {
			apply_shadow("sync_directory", [&] { shadow_.sync_directory(); });
			events_.push_back({"sync_directory", shadow_fs_->snapshot()});
		}
		if (f) { detail::fail(f->error, "faultlab recorder"); }
	}

	std::shared_ptr<journal::IO> real_;
	filesystem* shadow_fs_;
	io shadow_;
	std::string directory_;
	std::mutex mu_;
	std::vector<rule> rules_;
	std::vector<event> events_;
};

}  // namespace kronuz::faultlab
