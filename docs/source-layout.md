# Source layout

What each source file holds, so a change starts in the right one. faultlab is header-only; the
headers are under `include/faultlab/`, and users include `faultlab/faultfs.h`,
`faultlab/recorder.h`, `faultlab/scenario.h` or `faultlab/events.h`.

## faultfs

`faultfs.h` is the umbrella: it holds the description of the crash and media model (what a crash
keeps, ordering points, faults, media faults, shared mappings) and includes its four pieces in
dependency order. Each piece compiles alone.

| File | Holds |
|---|---|
| `faultfs/types.h` | The options: `op`, `fault`, `media`, the crash picks and `crash_policy`; the sync calls by flush mode and platform (`flush_mode`, `sync_platform`, `sync_site`, `sync_call`, `call_for`, `persists`). The per-file state, `detail::inode` (current, durable, staged and lost bytes, mapped sectors, leases, reference counts), `detail::shared_region`, the error and path helpers, and the coin sites crash() can force. |
| `faultfs/filesystem.h` | `class filesystem`, the model: the inodes, the current and durable namespaces, the namespace log and ordering points, reference counting and collection, fault rules, capacity, power cuts, `crash()`'s rebuild, `kill()`, `snapshot()` and moves. Also `detail::crash_access`, `detail::model_access` and `detail::inspection`, and the `FAULTLAB_CHECK_COUNTS` switch its count checks read. |
| `faultfs/file.h` | `class file`, the `journal::File`: reads with media faults, writes, truncates, preallocations, syncs and their faults, leases, staging at an ordering point, and its read and shared mappings. |
| `faultfs/io.h` | `class io`, the `journal::IO` of one directory: opens, creates, replace, link and remove, scans and inspections, `order_files_and_directory`, `sync_directory` and owner locks. |

`faultfs/filesystem.h` is 767 lines, most of it one class whose members share the model's private
state; it stays whole, since a class body is never cut.

## Files over 1,000 lines kept whole

None: after this split no file in the repository is over 1,000 lines. The largest are
`test/faultfs_test.cc` (858 lines) and `faultfs/filesystem.h` (767).
