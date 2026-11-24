# How faultfs rebuilds a crash state

faultfs models the durable sync contract of completion's `journal::IO`: what a sync, a directory
sync and an ordering point promise, and nothing a particular filesystem adds. `crash(seed,
policy)` picks one state that contract allows; the same seed and policy always pick the same one.

## What the model keeps

```
inode:   current bytes   durable bytes   staged[epoch]   at_mark[epoch]   lost pages
names:   current map path -> inode       durable map path -> inode
log:     namespace ops since the last directory sync (create, replace, link, remove)
marks:   ordering points: log entries before each are staged ahead of those after
```

A write changes `current` and marks the file dirty. A file sync makes `durable = current`. A
directory sync makes the directory's names durable and clears its log entries; it settles staged
data device-wide first (ruling R2). An ordering point (`order_files_and_directory`) closes an
epoch: each file's bytes as of that point are staged, and the namespace ops before it are ordered
ahead of the ops after it. A file staged after another thread's directory sync already settled its
point stays staged with no point; the next directory sync settles it. A replace onto its own name
changes nothing, live, in replay or when settled.

An inode lives while a current name, a durable name, a logged op or a handle references it. Each
inode counts those references: every change to a namespace or the log adjusts the inodes it names,
and one whose count falls to zero waits in an unreferenced set. Each remove, replace, directory
sync and crash collects that set, freeing what no handle holds. A directory sync makes durable only
the paths its directory's logged ops touched, since its durable names differ from its current ones
nowhere else, and `used()` is a running total each resizing call updates. So an operation costs
what it touches, not the live inode count; a crash, a settled ordering point and a snapshot recount
everything, as they rebuild it. Built with `FAULTLAB_CHECK_COUNTS=1`, as faultfs_test is, every
collection checks the counts against a full recount and each directory sync its durable names.

## Plain and full syncs

`set_flush_mode(mode, platform)` names the sync calls completion's PosixIO makes, so a sweep runs
a consumer in either flush mode. Each sync site makes one call, and the call decides what the
model keeps:

| Platform | Mode | `File::sync` | `File::sync_data` | `sync_directory` | ordering point |
|---|---|---|---|---|---|
| macOS | full | F_FULLFSYNC | F_FULLFSYNC | F_FULLFSYNC | F_FULLFSYNC |
| macOS | plain | fsync | fsync | fsync | F_BARRIERFSYNC |
| Linux | either | fsync | fdatasync | fsync | fsync |
| other | either | fsync | fsync | fsync | fsync |

| Call | Process crash (`kill`) | Power cut (`crash`) |
|---|---|---|
| F_FULLFSYNC; fsync and fdatasync off macOS | kept | kept: the bytes or names become durable, with what the ordering points staged (a device flush) |
| fsync on macOS | kept | may be dropped, as unsynced data: the bytes sit in the drive's cache |
| F_BARRIERFSYNC | kept | orders only, as every ordering point does |

The default is full on the host, so a user that never sets a mode sees the calls it always saw.
An ordering point stages whatever its call, since the contract promises it only orders.

A plain fsync on macOS pushes the file's synced bytes: they wait staged at the next ordering
point's epoch (the file joins `strays_`), so that point orders them ahead of later changes, as a
device-wide barrier orders everything the drive received before it. A file written again
between its push and the point records its bytes at the point in `at_mark`, which `as_of`
prefers at one epoch, so a crash cut at that point may keep the later write. A plain directory
sync changes nothing. Nothing settles until a full sync flushes the device, which settles every
push and every staged epoch; in plain mode on macOS the epochs and their staged copies
accumulate until a crash. A failed plain sync drops the unsynced bytes and reads back what the
drive holds, the last push.

## What `crash` draws, in order

1. **Epochs.** One coin per ordering point, in order, until the first lost one: the epochs before
   it survive whole (their staged bytes and namespace ops), nothing after it is ordered ahead.
2. **Names.** The log replays in order; an entry inside a surviving epoch survives, a later one
   by its own coin. A rename moves atomically. A `link` survives only when its source still names
   the same inode and its target is free; a remove of an inode whose link was lost is dropped, and
   a rename onto such an inode is dropped too, so no surviving name loses its last link (R1).
3. **Size.** Per dirty file still reachable: the durable size or the current one by coin, or, with
   `size_pick::sector`, one draw among the lower size, every 512-byte boundary between and the
   higher size (R5).
4. **Sectors.** Per 512-byte sector inside the size, a coin: the sector's old bytes or its new
   ones. Past the durable size, new bytes or zeros (growth that persisted without its data).
   Pages a failed sync with `keep` lost read their durable bytes again.

A fixed pick draws its coin and discards it, so it keeps the stream; only a size or epoch pick
changes how many coins follow, because the sector loop runs to the size and the epoch loop stops at
the first lost epoch.

## Shared mappings

`map_shared` hands out a word buffer. Its stores reach the model when any later call folds it
(every call on the file system folds first), and at `kill`, `snapshot` and `crash`. A sync or
write refreshes live buffers from the model. So the model sees what a page cache would: the
mapping's stores become file bytes, and a crash may tear them like any write.

io.h promises mapped stores no durability on macOS, so `File::sync` does not make them durable:
each sector a store changed keeps its bytes without the stores as its durable bytes, and a crash
tears it until a `write_at` over the whole sector or an ordering point that stages the file (the
design orders stores as writes made there). A consumer that relies on `sync` for mapped words
fails here. Once the power is off, a fold applies nothing: a store made after the cut never
survives.

## The recorder

`recorder` runs each call on a real IO and then on a shadow faultfs, and snapshots the shadow
after each mutating primitive. A cut after event i is that snapshot crashed with a seed and a
policy, or killed. `verify` compares the shadow's current names and bytes with the real
directory, so a write or link that bypassed the seam fails the run. Mapped stores reach only the
real file, so a mapped file verifies by length.

## Why sector tears and not write tears

The contract promises nothing about a write's atomicity, so faultfs tears per sector, the
smallest unit a device writes atomically. journal's MemoryIO and storage's CrashIO tear at write
boundaries over block appends, which `size_pick::sector` covers (A3). Unaligned CrashIO appends
reach states no sector pick does, sizes at a write boundary inside a sector (R7); a consumer that
needs them adds a write-boundary cut.

MemoryIO's states are not computed in the test: `test/fixtures/memory_io_states.txt` records them
for every sequence `sector_test` checks, at journal:a412175, so faultlab fetches nothing from
journal. `test/fixtures/regenerate_memory_io.sh` (`-DFAULTLAB_REGENERATE_MEMORY_IO=ON`, which builds
`faultlab_memory_io_gen`) rewrites the file from the live MemoryIO.
