// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs's directory, class io (a journal::IO): opens, creates, replace, link and remove, scans,
// inspections, ordering points, directory syncs and owner locks.
#pragma once

#include "completion/io.h"
#include "faultlab/faultfs/file.h"
#include "faultlab/faultfs/filesystem.h"
#include "faultlab/faultfs/types.h"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kronuz::faultlab {

class io final : public journal::IO {
public:
	// The IO of `directory` ("" the root, else one make_directory made) on `fs`.
	explicit io(filesystem& fs, std::string directory = {}) : fs_(&fs), directory_(std::move(directory)) {
		std::lock_guard lock(fs_->mu_);
		if (!fs_->directory_exists(directory_)) { detail::fail(ENOENT, "faultfs io directory"); }
	}

	using journal::IO::acquire_owner;
	std::unique_ptr<journal::OwnerLock> acquire_owner(bool create) override { return lock_owner(journal::OwnerLockOptions::legacy_file(create)); }
	std::unique_ptr<journal::OwnerLock> acquire_directory_owner() override { return lock_owner(journal::OwnerLockOptions::legacy_directory()); }
	journal::FilesystemClass directory_filesystem() const override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		return fs_->class_;
	}
	journal::DirectoryTrust directory_trust() const override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		return fs_->trust_;
	}
	std::unique_ptr<journal::File> open_existing(std::string_view name) override { return open(name, false); }
	std::unique_ptr<journal::File> open_existing_read_only(std::string_view name) override { return open(name, true); }
	// Null when the name is missing or a directory, as io.h's reclaim opens promise.
	std::unique_ptr<journal::File> open_reclaim_candidate(std::string_view name) override { return open(name, false, true); }
	std::unique_ptr<journal::File> open_reclaim_candidate_read_only(std::string_view name) override { return open(name, true, true); }
	void open_existing_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable file open unsupported"); }
	bool open_reclaim_candidate_read_only_into(std::string_view, journal::File&) override { throw std::logic_error("reusable candidate open unsupported"); }
	std::unique_ptr<journal::File> create_exclusive(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::create);
		if (f && !f->after) { detail::fail(f->error, "faultfs create"); }
		if (exists(name)) { detail::fail(EEXIST, "faultfs create"); }
		auto node = make_locked(name);
		if (f) { detail::fail(f->error, "faultfs create"); }
		return std::make_unique<file>(*fs_, std::move(node));
	}
	void replace(std::string_view source, std::string_view destination) override {
		detail::check_name(source);
		detail::check_name(destination);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::replace);
		if (f && !f->after) { detail::fail(f->error, "faultfs replace"); }
		if (is_directory(source) || is_directory(destination)) { detail::fail(EISDIR, "faultfs replace"); }
		auto it = fs_->names_.find(path(source));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs replace"); }
		const std::uint64_t id = it->second;
		fs_->log({op::replace, path(source), path(destination), id});
		fs_->erase_name(fs_->names_, it);
		fs_->set_name(fs_->names_, path(destination), id);
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs replace"); }
	}
	// A second name for source's file; never replaces (EEXIST).
	void link(std::string_view source, std::string_view destination) override {
		detail::check_name(source);
		detail::check_name(destination);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::link);
		if (f && !f->after) { detail::fail(f->error, "faultfs link"); }
		if (is_directory(source)) { detail::fail(EPERM, "faultfs link"); }
		auto it = fs_->names_.find(path(source));
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs link"); }
		if (exists(destination)) { detail::fail(EEXIST, "faultfs link"); }
		const std::uint64_t id = it->second;
		fs_->set_name(fs_->names_, path(destination), id);
		fs_->log({op::link, path(source), path(destination), id});
		if (f) { detail::fail(f->error, "faultfs link"); }
	}
	// Missing names are an idempotent success; a directory fails with EPERM, as unlink does.
	void remove(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::remove);
		if (f && !f->after) { detail::fail(f->error, "faultfs remove"); }
		if (is_directory(name)) { detail::fail(EPERM, "faultfs remove of a directory"); }
		if (auto it = fs_->names_.find(path(name)); it != fs_->names_.end()) {
			fs_->log({op::remove, path(name), {}, it->second});
			fs_->erase_name(fs_->names_, it);
			fs_->collect();
		}
		if (f) { detail::fail(f->error, "faultfs remove"); }
	}
	std::unique_ptr<journal::DirectoryCursor> scan_directory() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return std::make_unique<cursor>(fs_->entries(directory_));
	}
	std::optional<journal::EntryFootprint> entry_footprint(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		if (is_directory(name)) { return journal::EntryFootprint{journal::EntryKind::Directory, 0, std::nullopt}; }
		auto it = fs_->names_.find(path(name));
		if (it == fs_->names_.end()) { return std::nullopt; }
		return journal::EntryFootprint{journal::EntryKind::Regular, fs_->inodes_.at(it->second)->current.size(), std::nullopt};
	}
	std::optional<journal::EntryInspection> inspect_entry(std::string_view name) override {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		if (is_directory(name)) {
			journal::EntryInspection out;
			out.footprint = {journal::EntryKind::Directory, 0, std::nullopt};
			out.device = 1;
			out.inode = (std::uint64_t(1) << 63) | std::uint64_t(std::distance(fs_->directories_.begin(), fs_->directories_.find(path(name))));
			out.owner = ::geteuid();
			out.links = 1;
			out.permissions = 0700;
			return out;
		}
		auto it = fs_->names_.find(path(name));
		if (it == fs_->names_.end()) { return std::nullopt; }
		return detail::inspection(*fs_->inodes_.at(it->second), fs_->links_of(it->second));
	}
	// The files' bytes and the namespace so far, staged ahead of whatever follows.
	void order_files_and_directory(std::span<journal::File* const> files) override {
		std::size_t epoch = 0;
		{
			std::lock_guard lock(fs_->mu_);
			fs_->fold_locked();
			epoch = fs_->marks_.size();
			fs_->marks_.push_back(fs_->log_.size());
		}
		bool staged = false;
		for (auto* f : files) {
			if (auto* own = dynamic_cast<file*>(f)) {
				own->stage(epoch);
				staged = true;
			} else {
				f->sync();
			}
		}
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		const fault* f = fs_->check(op::sync_directory);
		if (f && !f->after) {
			if (!staged) { fs_->drop_last_mark(); }
			detail::fail(f->error, "faultfs order_files_and_directory");
		}
		if (f) { detail::fail(f->error, "faultfs order_files_and_directory"); }
	}
	// This directory's namespace changes durable, with what the ordering points staged.
	void sync_directory() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const fault* f = fs_->check(op::sync_directory);
		if (f && !f->after) { detail::fail(f->error, "faultfs sync_directory"); }
		fs_->settle_directory(directory_);
		fs_->collect();
		if (f) { detail::fail(f->error, "faultfs sync_directory"); }
	}

protected:
	// owner.lock (file), the directory (directory), or both; busy is EWOULDBLOCK in every mode.
	// Creating owner.lock is logged as a create, but is no mutation and meets no fault rule.
	std::unique_ptr<journal::OwnerLock> lock_owner(const journal::OwnerLockOptions& options) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const bool want_file = options.mode != journal::OwnerLockMode::directory;
		const bool want_directory = options.mode != journal::OwnerLockMode::file;
		if ((want_file && fs_->owners_.count(directory_)) || (want_directory && fs_->directory_owners_.count(directory_))) {
			detail::fail(EWOULDBLOCK, "faultfs owner lock held");
		}
		if (want_file) {
			const bool create = options.owner_file == journal::OwnerFile::create_exclusive;
			if (create == exists("owner.lock")) { detail::fail(create ? EEXIST : ENOENT, "faultfs owner lock"); }
			if (create) { make_locked("owner.lock"); }
			fs_->owners_.insert(directory_);
		}
		if (want_directory) { fs_->directory_owners_.insert(directory_); }
		return std::make_unique<owner>(*fs_, directory_, want_file, want_directory);
	}

private:
	std::unique_ptr<journal::File> open(std::string_view name, bool read_only, bool candidate = false) {
		detail::check_name(name);
		std::lock_guard lock(fs_->mu_);
		enter();
		auto it = fs_->names_.find(path(name));
		if (candidate && (is_directory(name) || it == fs_->names_.end())) { return nullptr; }
		if (is_directory(name)) { detail::fail(EISDIR, "faultfs open"); }
		if (it == fs_->names_.end()) { detail::fail(ENOENT, "faultfs open"); }
		return std::make_unique<file>(*fs_, fs_->inodes_.at(it->second), read_only);
	}
	struct owner final : journal::OwnerLock {
		owner(filesystem& fs, std::string directory, bool file, bool dir) : fs_(&fs), directory_(std::move(directory)), file_(file), dir_(dir), epoch_(fs.epoch_) {}
		~owner() override {
			std::lock_guard lock(fs_->mu_);
			if (epoch_ != fs_->epoch_) { return; }
			if (file_) { fs_->owners_.erase(directory_); }
			if (dir_) { fs_->directory_owners_.erase(directory_); }
		}
		filesystem* fs_;
		std::string directory_;
		bool file_, dir_;
		std::uint64_t epoch_;
	};
	struct cursor final : journal::DirectoryCursor {
		explicit cursor(std::vector<std::string> names) : names_(std::move(names)) {}
		std::optional<std::string> next() override {
			if (at_ == names_.size()) { return std::nullopt; }
			return names_[at_++];
		}
		std::vector<std::string> names_;
		std::size_t at_ = 0;
	};
	void enter() {
		++fs_->calls_;
		fs_->fold_locked();
	}
	std::string path(std::string_view name) const { return detail::join(directory_, name); }
	bool is_directory(std::string_view name) const { return fs_->directories_.count(path(name)) != 0; }
	bool exists(std::string_view name) const { return fs_->names_.count(path(name)) != 0 || is_directory(name); }
	std::shared_ptr<detail::inode> make_locked(std::string_view name) {
		const std::uint64_t id = fs_->next_id_++;
		auto node = std::make_shared<detail::inode>();
		node->id = id;
		fs_->inodes_[id] = node;
		fs_->set_name(fs_->names_, path(name), id);
		fs_->log({op::create, path(name), {}, id});
		return node;
	}
	filesystem* fs_;
	std::string directory_;
};

}  // namespace kronuz::faultlab
