// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// faultfs: an in-memory kronuz::journal::IO that models what storage keeps across power loss,
// for crash testing. One filesystem is one device; an io is one directory on it. Each file has
// current bytes and durable bytes; each directory has a current namespace and a durable one.
// File::sync makes a file's bytes durable, sync_directory makes that directory's namespace
// durable. crash() rebuilds the state a power loss could leave:
//
//   - each 512-byte sector written since the file's last sync keeps its old bytes, takes its
//     new ones, or (beyond the durable size) reads as zeros; the size is the durable one or the
//     current one (or, with size_pick::sector, any sector boundary between them);
//   - each create, link, rename and unlink since its directory's last sync survives or not,
//     replayed in order, a rename atomically; an op whose source no longer exists drops, and an
//     op that drops a name of a file whose earlier link dropped drops too.
//
// IO::order_files_and_directory stages the files' bytes and the namespace changes so far ahead
// of every later change, without making them durable: after it, a create, link, rename or unlink
// may survive a crash only if every staged byte and namespace change survived too; the next
// sync_directory makes the staged bytes durable with the namespace, and so does the next
// File::sync (the barrier's second phase: a sync that returns has flushed what the barrier
// ordered ahead of it). An ordering point is device-wide. A write to a staged file after the
// ordering point is not ordered: it stays unsynced over the staged bytes until that file's own
// sync, and never survives without them. Ordering points chain: a crash keeps the epochs before
// some point whole, that point's epoch in part, and nothing after it.
//
// Faults: a rule fails the n-th operation of a kind, before or after its effect, with EIO or
// ENOSPC; a capacity fails writes and preallocations that grow the total past it with ENOSPC;
// short writes return part of the request. A failed File::sync drops the file's unsynced bytes;
// a later sync succeeds; one that failed after its effect made the flushed bytes durable. Handles
// opened before a crash, a kill() or a power_on() fail with EIO.
//
// Media faults model unreliable storage on reads: a read overlapping a file's byte range fails
// with EIO (a latent sector error) or returns one bit flipped in that range, the next `times`
// reads (transient) or every read (sticky, times 0). The stored bytes never change.
//
// Shared mappings (File::map_shared) are buffers folded into the file at every call into the
// filesystem, before that call takes effect, and at crash, kill and snapshot: each changed
// sector is written as a write there would be, bypassing faults, the power counter and
// mutations(). Do not store into a mapping while another thread calls in.
#pragma once

#include "faultlab/faultfs/types.h"       // options and per-file state
#include "faultlab/faultfs/filesystem.h"  // the model and crash()
#include "faultlab/faultfs/file.h"        // the open file
#include "faultlab/faultfs/io.h"          // the directory
