// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs's open file, class file (a journal::File): reads with media faults, writes, truncates,
// preallocations, syncs, leases, staging at an ordering point, and its read and shared mappings.
#pragma once

#include "completion/io.h"
#include "faultlab/faultfs/filesystem.h"
#include "faultlab/faultfs/types.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace kronuz::faultlab {

class file final : public journal::File {
public:
	// A read-only file reads, syncs and closes; write_at, truncate, preallocate and map_shared fail.
	file(filesystem& fs, std::shared_ptr<detail::inode> node, bool read_only = false) : fs_(&fs), node_(std::move(node)), epoch_(fs.epoch_), id_(fs.next_handle_++), read_only_(read_only) {}
	~file() override {
		std::lock_guard lock(fs_->mu_);
		node_->leases.erase(id_);
	}

	std::uint64_t size() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return node_->current.size();
	}
	std::size_t read_at(std::uint64_t offset, std::span<char> bytes) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		const std::string& d = node_->current;
		if (offset >= d.size()) { return 0; }
		const std::size_t n = std::min<std::uint64_t>(bytes.size(), d.size() - offset);
		std::memcpy(bytes.data(), d.data() + offset, n);
		for (auto& m : fs_->media_) {
			if (m.node != node_ || m.end <= offset || m.begin >= offset + n || (!m.sticky && m.left == 0)) { continue; }
			if (!m.sticky) { --m.left; }
			++fs_->media_hits_;
			if (m.kind == media::read_error) { detail::fail(EIO, "faultfs read"); }
			const std::uint64_t at = std::max(m.begin, offset);
			bytes[at - offset] = char(bytes[at - offset] ^ (1 << (at % 8)));
		}
		return n;
	}
	std::size_t write_at(std::uint64_t offset, std::string_view bytes) override {
		std::lock_guard lock(fs_->mu_);
		const accounted resized{*fs_, *node_};
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs write"); }
		const fault* f = fs_->check(op::write);
		if (f && !f->after) { detail::fail(f->error, "faultfs write"); }
		std::size_t n = bytes.size();
		if (fs_->short_ && n > fs_->short_) { n = fs_->short_; }
		const std::uint64_t end = offset + n;
		const std::uint64_t held = node_->footprint();
		if (end > held && fs_->used_locked() + (end - held) > fs_->capacity_) { detail::fail(ENOSPC, "faultfs write"); }
		node_->note_write(fs_->marks_.size());
		if (end > node_->current.size()) { node_->current.resize(end, '\0'); }
		std::memcpy(node_->current.data() + offset, bytes.data(), n);
		node_->touch(offset, end);
		if (fs_->damage_left_ > 0 && n >= fs_->damage_min_ && fs_->damage_at_ < n) {
			node_->current[offset + fs_->damage_at_] ^= 0x10;
			--fs_->damage_left_;
			++fs_->damaged_;
		}
		node_->overwrite_unmapped(offset, end);
		node_->dirty = true;
		fs_->refresh(*node_, offset, end);
		if (f) { detail::fail(f->error, "faultfs write"); }
		return n;
	}
	void truncate(std::uint64_t size) override {
		std::lock_guard lock(fs_->mu_);
		const accounted resized{*fs_, *node_};
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs truncate"); }
		const fault* f = fs_->check(op::truncate);
		if (f && !f->after) { detail::fail(f->error, "faultfs truncate"); }
		if (node_->mappings && size < node_->mapped) {
			std::fprintf(stderr, "faultfs: a file truncated below a live mapping\n");
			std::abort();
		}
		node_->note_write(fs_->marks_.size());
		node_->current.resize(size, '\0');
		node_->truncate_unmapped(size);
		node_->touch(size, ~std::uint64_t(0));
		node_->reserved = std::min(node_->reserved, size);
		node_->dirty = true;
		if (f) { detail::fail(f->error, "faultfs truncate"); }
	}
	// Grows the file to at least offset + length, zeros, and reserves that extent against the
	// capacity: a mutation, failed by a fault rule, the power or a full device before any effect.
	void preallocate(std::uint64_t offset, std::uint64_t length) override {
		if (length > ~std::uint64_t{0} - offset) { throw std::length_error("preallocation exceeds the file range"); }
		std::lock_guard lock(fs_->mu_);
		const accounted resized{*fs_, *node_};
		enter();
		if (read_only_) { detail::fail(EBADF, "faultfs preallocate"); }
		const fault* f = fs_->check(op::preallocate);
		if (f && !f->after) { detail::fail(f->error, "faultfs preallocate"); }
		const std::uint64_t end = offset + length;
		const std::uint64_t held = node_->footprint();
		if (end > held && fs_->used_locked() + (end - held) > fs_->capacity_) { detail::fail(ENOSPC, "faultfs preallocate"); }
		if (end > node_->current.size()) {
			const std::uint64_t was = node_->current.size();
			node_->note_write(fs_->marks_.size());
			node_->current.resize(end, '\0');
			node_->touch(was, end);
			node_->dirty = true;
		}
		node_->reserved = std::max(node_->reserved, end);
		if (f) { detail::fail(f->error, "faultfs preallocate"); }
	}
	// A copy of the file's bytes, registered on the inode while it lives.
	std::shared_ptr<const journal::Mapping> map() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (!fs_->map_files_) { return nullptr; }
		return std::make_shared<mapping>(*fs_, node_);
	}
	// A buffer over [0, length), folded into the file at every call into the filesystem.
	std::shared_ptr<journal::SharedMapping> map_shared(std::uint64_t length) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (read_only_) { detail::fail(EACCES, "faultfs map_shared"); }
		if (length > node_->current.size()) { detail::fail(EINVAL, "faultfs map_shared past the end"); }
		return std::make_shared<shared_mapping>(*fs_, node_, length);
	}
	void sync() override {
		if (const auto us = fs_->sync_latency_.load(std::memory_order_relaxed)) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
		std::lock_guard lock(fs_->mu_);
		const accounted resized{*fs_, *node_};
		enter();
		if (const fault* f = fs_->check(op::sync)) {
			if (f == &filesystem::power_off_) { detail::fail(f->error, "faultfs sync"); }
			// A failed fsync loses the unsynced bytes; reads see the durable ones, or with `keep`
			// still the cached ones. One that failed after its effect flushed them, and what an
			// ordering point staged before them.
			if (f->after) {
				node_->durable = node_->synced();
				node_->staged.clear();
				node_->at_mark.clear();
				fs_->settle_staged();
			}
			if (f->keep && !f->after) {
				node_->lose_unsynced();
				node_->dirty = false;
			} else {
				node_->drop_unstaged();
				fs_->refresh(*node_, 0, node_->current.size());
			}
			detail::fail(f->error, "faultfs sync");
		}
		node_->durable = node_->synced();
		node_->dirty = !node_->unmapped.empty();
		node_->staged.clear();
		node_->at_mark.clear();
		fs_->settle_staged();
	}
	journal::EntryInspection inspect() override {
		std::lock_guard lock(fs_->mu_);
		enter();
		return detail::inspection(*node_, fs_->links_of(node_->id));
	}
	// A per-inode lease: shared ones coexist, an exclusive one excludes all. Released when this
	// handle closes or is destroyed, and by kill() and crash().
	bool try_lease(journal::LeaseMode mode) override {
		std::lock_guard lock(fs_->mu_);
		enter();
		if (node_->leases.count(id_)) { throw std::logic_error("faultfs: a lease on a handle that holds one"); }
		++fs_->ops_[std::size_t(op::lease)];
		const fault* f = fs_->rule_for(op::lease);
		if (f && !f->after) { detail::fail(f->error, "faultfs lease"); }
		bool free = true;
		for (const auto& [_, held] : node_->leases) { free = free && mode == journal::LeaseMode::Shared && held == journal::LeaseMode::Shared; }
		if (free) { node_->leases[id_] = mode; }
		if (f) { detail::fail(f->error, "faultfs lease"); }
		return free;
	}
	void close() override {
		std::lock_guard lock(fs_->mu_);
		++fs_->calls_;
		fs_->fold_locked();
		node_->leases.erase(id_);
		closed_ = true;
	}

	// Re-accounts the file's share of used() when a call that may resize it ends, thrown or not.
	struct accounted {
		filesystem& fs;
		detail::inode& node;
		~accounted() { fs.account(node); }
	};
	// Stages the bytes ahead of later changes for ordering point `epoch` (see
	// io::order_files_and_directory); durable at the next sync_directory.
	void stage(std::size_t epoch) {
		std::lock_guard lock(fs_->mu_);
		const accounted resized{*fs_, *node_};
		enter();
		if (const fault* f = fs_->check(op::sync)) {
			if (f != &filesystem::power_off_) {
				node_->drop_unstaged();
				fs_->refresh(*node_, 0, node_->current.size());
			}
			detail::fail(f->error, "faultfs stage");
		}
		if (node_->dirty) {
			node_->staged.emplace_back(epoch, node_->flushed());
			if (epoch >= fs_->marks_.size()) { fs_->strays_.insert(node_->id); }  // settled at the next sync_directory
		}
		node_->unmapped.clear();  // the point orders the mapped stores as writes made here
	}

private:
	class mapping final : public journal::Mapping {
	public:
		mapping(filesystem& fs, std::shared_ptr<detail::inode> node) : fs_(&fs), node_(std::move(node)), copy_(node_->current) {
			bytes_ = copy_;
			++node_->mappings;
			node_->mapped = std::max<std::uint64_t>(node_->mapped, copy_.size());
		}
		~mapping() override {
			std::lock_guard lock(fs_->mu_);
			if (--node_->mappings == 0) { node_->mapped = 0; }
		}

	private:
		filesystem* fs_;
		std::shared_ptr<detail::inode> node_;
		std::string copy_;
	};
	class shared_mapping final : public journal::SharedMapping {
	public:
		shared_mapping(filesystem& fs, std::shared_ptr<detail::inode> node, std::uint64_t length) : fs_(&fs) {
			region_.node = std::move(node);
			region_.words.assign(region_.node->current.begin(), region_.node->current.begin() + std::ptrdiff_t(length));
			view(std::span<char>(region_.words));
			++region_.node->mappings;
			region_.node->mapped = std::max<std::uint64_t>(region_.node->mapped, length);
			fs_->shared_.push_back(&region_);
		}
		// Unmapping keeps the stores: they fold, unless a crash or kill detached the mapping.
		~shared_mapping() override {
			std::lock_guard lock(fs_->mu_);
			auto it = std::find(fs_->shared_.begin(), fs_->shared_.end(), &region_);
			if (it == fs_->shared_.end()) { return; }
			fs_->fold(region_);
			fs_->shared_.erase(it);
			if (--region_.node->mappings == 0) { region_.node->mapped = 0; }
		}

	private:
		filesystem* fs_;
		detail::shared_region region_;
	};
	// Every call: counted, stale and closed handles refused, shared mappings folded first.
	void enter() {
		++fs_->calls_;
		if (epoch_ != fs_->epoch_) { detail::fail(EIO, "faultfs handle from before a crash"); }
		if (closed_) { detail::fail(EBADF, "faultfs closed handle"); }
		fs_->fold_locked();
	}
	filesystem* fs_;
	std::shared_ptr<detail::inode> node_;
	std::uint64_t epoch_;
	std::uint64_t id_;
	bool read_only_;
	bool closed_ = false;
};

}  // namespace kronuz::faultlab
