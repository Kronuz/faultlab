# faultlab API

Namespace `kronuz::faultlab`; headers under `include/faultlab/`. The C++ half needs only
completion's `completion/io.h` (`kronuz::journal::IO`, `File`). The Python half is under
`python/faultlab/`, stdlib only, Python 3.12.

## faultfs.h

### `filesystem`

The model: an in-memory device holding a root directory and its subdirectories. Move-only.

| Member | What it does |
|---|---|
| `filesystem(capacity = unlimited)` | An empty device; `FAULTLAB_MAP=1` turns `map_files` on. |
| `make_directory(path)` | A directory (`"a"` or `"a/b"`) whose parent exists, durable at once. Setup, not a mutation. |
| `crash(seed, crash_policy = {})` | Power loss: rebuilds a state the device could hold, chosen by `seed` and the policy. Handles go stale; owner locks and leases release; shared mappings detach. |
| `kill()` | Process death: handles stale, locks and leases released, mapping stores kept; current and durable bytes stay. |
| `snapshot() const` | A deep copy without handles, locks, leases or mappings, mapping stores folded first. |
| `power_off_after(n)` / `powered_off()` | After `n` more mutations, each one fails with `EIO` before its effect, until `crash()` or `power_on()`. |
| `power_on()` | Ends the cut as a process crash, where `crash()` ends it as a power loss: handles go stale and locks, leases and mappings release as at `kill()`, every byte stays, synced or not, and operations succeed again. |
| `mutations()`, `syncs()`, `calls()` | Counters: mutating calls (crash points), syncs that reached the device, every IO and File call. |
| `operations(op)` | Calls of one kind so far, failed ones included: `mutations()` by kind, and `try_lease` calls for `op::lease`. |
| `sync_latency(d)` | Each `File::sync` (and `sync_data`) then waits at least `d` before its effect, outside the model's lock, as a slow device flush; `{}` ends it. |
| `add_fault(fault)` / `clear_faults()` | Fails the `nth` call of `fault.kind` with `fault.error`, before or `after` its effect; a sync with `keep` loses its pages (Linux writeback-error semantics). |
| `add_media_fault(path, media, begin, end, times = 0)` / `media_hits()` | Reads overlapping `[begin, end)` fail (`read_error`) or flip a bit (`bit_flip`). |
| `short_writes(max_bytes)` | Each write stores at most `max_bytes` and returns the count. |
| `damage_writes(min, at, count = 1)` / `damaged_writes()` | Silent damage: a flipped bit `at` bytes into each of the next `count` writes of at least `min` bytes. |
| `set_flush_mode(flush_mode, sync_platform = host_platform)` / `sync_call_at(sync_site)` | The flush mode (`full`, `plain`) and platform (`macos`, `linux_kernel`, `other`) whose sync calls the model makes, and the call (`fsync`, `fdatasync`, `full_fsync`, `barrier_fsync`) at a site (`file`, `data`, `directory`, `order`). Default: `full` on the host. macOS's plain fsync survives `kill()` but a `crash()` may drop it; every other file or directory sync persists. `call_for(site, mode, platform)` and `persists(call, platform)` are the table ([crash-model.md](mechanisms/crash-model.md#plain-and-full-syncs)). |
| `set_capacity(bytes)` / `used()` | `ENOSPC` past the capacity; `used` counts bytes and preallocated extents. |
| `classify(FilesystemClass)`, `set_trust(DirectoryTrust)`, `map_files(bool)` | What `io` reports for `directory_filesystem`, `directory_trust`, and whether `File::map` maps. |
| `view(path) const` | The current bytes of a file, valid until the next change to it. |
| `list(directory = {}) const` | The names in a directory, files and subdirectories, sorted. |

### `crash_policy`

Each field fixes one family of coins; `coin` (the default) flips it. A fixed pick still draws and
discards its coin, so it keeps the stream, except as below.

| Field | Picks | Fixed meaning |
|---|---|---|
| `epochs` | `coin`, `none`, `all` | Ordering points kept whole: none, or all of them. |
| `names` | `coin`, `durable`, `all`, `additions` | Namespace ops the epoch cut admits: none, all, or only those that add a name. |
| `size` | `coin`, `durable`, `current`, `sector` | Each dirty file's size: the old one, the new one, or any sector boundary between (one draw). |
| `sectors` | `coin`, `old`, `fresh` | Each unsynced 512-byte sector's bytes: old, or new. |

A size pick changes how many sector coins follow (their count depends on the size) and an epoch
pick how many epoch coins are drawn (the loop stops at the first lost epoch), so either moves the
later draws; `sector_pick` and `name_pick` keep the stream.

### `io` and `file`

`io(filesystem&, directory = {})` is a `journal::IO` over one directory of the model (`ENOENT`
when it is missing); its files are `journal::File`s. The whole seam is modeled:

- Opens, `create_exclusive`, `replace` (atomic rename), `link` (never replaces: `EEXIST`),
  `remove`, scans, footprints and `inspect_entry` (device 1, the effective uid, mode 0600, 0700
  for a directory). A directory refuses `remove` and `link` (`EPERM`) and `open` (`EISDIR`). The
  reclaim-candidate opens (`open_reclaim_candidate`, its read-only form) return null for a missing
  name or a directory, as `io.h` says; the plain opens throw.
- `sync`, `sync_data` and `sync_directory`, each as its call in the flush mode; a `sync` that
  fails after its effect leaves the flushed bytes durable, with what an ordering point staged
  before them (pushed, with a plain call on macOS); the group syncs (`sync_files`, `sync_data_files`,
  `sync_files_and_directory`) are the seam's defaults over them; `order_files_and_directory` is
  the ordering point that stages each file ahead of later changes.
- `preallocate` (a mutation; reserves space), `map` (read-only, with `map_files`),
  `map_shared` (`EACCES` read-only, `EINVAL` past the end; its stores reach the model when a call
  folds them, nothing once the power is off, and `File::sync` leaves them tearable), `try_lease` (shared leases coexist, an exclusive one excludes), `inspect`.
- Owner locks, file and directory, every mode: `EWOULDBLOCK` when held, `EEXIST` and `ENOENT` as
  the options ask. A crash or kill releases them.

## recorder.h

| Member | What it does |
|---|---|
| `load(path)` | A durable model of a real directory: its subdirectories, files and bytes. |
| `materialize(fs, path)` | Writes a model's current names and bytes into a missing or empty directory (0700, files 0600). |
| `recorder(shared_ptr<IO> real, filesystem& shadow, directory = {})` | A `journal::IO` that runs each call on `real`, then on `shadow`'s `directory`. |
| `events()` | `{label, state}` per mutating primitive (`create x`, `write x`, `sync x`, `link a b`, `remove x`, `sync_directory`, `order_files_and_directory`, ...), event 0 `start`. |
| `note(label)` | A labelled event from a test hook. |
| `add_fault(fault)` | Fails the `nth` real call of a kind, before (nothing happens) or after (both happen, recorded). |
| `verify(real_directory, length_only = {})` | Throws unless the shadow's names and bytes equal the real directory's; `length_only` files (mapped) by length. |

A shadow that refuses what the real IO did throws `std::logic_error` ("diverged"). A group sync
calls the real IO once and the shadow per file, one event each.

## scenario.h and scenario.py

`scenario{name, seed, cut, pick, extra}`; `line()` prints
`faultlab/1 name=<word> seed=<u64> [cut=<u64>] [pick=<epochs>,<names>,<size>,<sectors>] [key=value...]`,
`parse(line)` reads it strictly (unknown version, missing name or seed, a repeated key, a bad
number or pick: `std::invalid_argument`, `ScenarioError` in Python). `random()` is the case's
one stream (`std::mt19937_64`, `random.Random`). `requested()` reads `FAULTLAB_SCENARIO`;
`selected()` is true for every case without it, else only for the case it names (its cut and pick
too, when given). `replay()` is `FAULTLAB_SCENARIO='<line>'`. `get(key)` reads an extra key.

## events.h

`event_log(path, scenario)` truncates `path` and writes the scenario line; each later line is
flushed and serialized by a mutex.

- `record(operation)`: `{client, op, key, value, level, outcome, time}` plus `node`, `launch`
  and `durable` when set. `op` is `write`, `read` or `get`; `outcome` is `invoke`, `ok`, `fail` or
  `info`; a write's `level` is `quorum` or `none`, a read's `prefix`, `linearizable` or `seal`.
- `fault(kind, time, args)`: one of `fault_kinds` (partition, heal, isolate, reset, cap, delay,
  loss, reorder, duplicate, pause, resume, kill, restart, power_cut, media, enospc), arguments
  by name.
- `note(kind, fields)`: a harness record (`{"kind": ...}`), which the checker sets aside.
- `now()`: seconds since the log opened. `encode(fields)`: one JSON object, keys in order.

## history.py

`python3 -I python/faultlab/history.py LOG [--level prefix|linearizable]` exits 0 when the
history passes, 1 with each violation and the replay line, 2 when the log does not parse.

| Name | What it does |
|---|---|
| `load_log(lines) -> Log` | Pairs each client's invocations with completions (an open one is `info`); returns `ops`, `faults`, `scenario`. |
| `load(lines) -> list[Op]` | `load_log(lines).ops`. |
| `check(ops, level=None, disabled=frozenset(), faults=None) -> list[str]` | The violations of the five checks, each with the faults inside its operations' interval. |
| `Recorder(path, scenario=None)` | Appends lines: `record(client, op, key, value, level, outcome, **extra)`, `fault(kind, **args)`. |

The checks, per key:

1. `written`: a read returns null or a value some write had begun to write.
2. `prefix`: one order of the writes explains every non-seal read as a prefix of it, monotonic
   per node launch and never behind a `quorum` write the node acknowledged.
3. `linearizable`: the writes and the `linearizable` reads linearize (Gibbons and Korach's zones).
4. `monotonic`: a node whose reads are marked `durable` never reads behind what it served, across
   its launches.
5. `seal`: a `seal` read returns a number no lower than any completed seal read or acknowledged
   write returned before it began.

## proxy.py

The portable network tier: a TCP link per directed node pair on 127.0.0.1, TLS-transparent.

| Name | What it does |
|---|---|
| `Mesh(log=None)` | Links for nodes numbered from 1; `log` is anything with `fault(kind, **args)` (`Recorder`). |
| `await start(peer_ports)`, `address(source, target)` | Starts the links; the address node `source` dials to reach `target`. |
| `partition(a, b)`, `isolate(n)` | Hold every byte and connection between `a` and `b`, or between `n` and every node, until healed. |
| `heal(a=None, b=None)` | Heals every link, `a`'s, or the pair's. |
| `reset(a, b)` | Aborts the pair's connections, as a peer's RST. |
| `cap(mbit)` | Caps each direction of every link; 0 lifts it. |
| `delay`, `loss`, `reorder`, `duplicate` | Raise `Unsupported`: netlab makes them. |
| `await close()` | Closes every link, then raises the first failure a forwarding task met. |

`python3 -I python/faultlab/proxy.py bandwidth` measures the cap against its target.

## netlab.py

The Linux network tier: a network namespace per node, a veth pair per pair of nodes, netem on each
end and an nftables set per namespace. Needs Linux and root; `unavailable()` says why a host cannot
run it. How it works: [mechanisms/netlab.md](mechanisms/netlab.md).

| Name | What it does |
|---|---|
| `Netlab(log=None, prefix="faultlab", runner=run)` | A lab for nodes numbered 1 to 254; `log` as Mesh's; `runner` runs its commands. |
| `await start(peer_ports)`, `open(peer_ports)` | Builds the namespaces and links; each node listens on its port in its own namespace. |
| `address(source, target)`, `ip(node, peer)` | The address `source` dials to reach `target`: `target`'s end of their link. |
| `namespace(node)`, `command(node, argv)`, `socket(node, kind)` | The node's namespace; `argv` run in it; an IPv4 socket made in it. |
| `partition(a, b)`, `isolate(n)` | Drop every packet between `a` and `b`, or between `n` and every node, until healed; TCP retransmits after. |
| `heal(a=None, b=None)` | Heals every link, `a`'s, or the pair's. |
| `reset(a, b)` | Destroys every TCP socket between `a` and `b` on both sides (`ss -K`), each sending its peer a RST; new connections work. |
| `cap(mbit, a=None, b=None)` | Caps each direction of every link, `a`'s, or the pair's; 0 lifts it. |
| `delay(ms, ...)`, `loss(p, ...)`, `reorder(p, ...)`, `duplicate(p, ...)` | netem on the same links; `delay` is per direction, `p` a probability; `reorder` needs a delay. |
| `await close()`, `teardown()` | Deletes the namespaces, and with them every veth, qdisc and table. |
| `sweep(prefix, everything=False)` | Deletes the namespaces of labs whose owner died, or all with `prefix`. |

`python3 -I python/faultlab/netlab.py sweep [--prefix P] [--all]` sweeps from the shell.

## disklab.py

The disk tier: a filesystem on a fault layer (linear, dm-delay or dm-flakey) over dm-log-writes
over two loop devices; the log replays to any flush, FUA write or mark. Needs Linux, root and the
three dm targets, so it runs in a VM; `unavailable()` says why a host cannot run it. How it works:
[mechanisms/disklab.md](mechanisms/disklab.md).

| Name | What it does |
|---|---|
| `Disklab(log=None, node=None, root=/var/tmp, prefix="faultlab", runner=run)` | A lab in `<root>/<prefix>-<pid>-<n>`; events carry `nodes=[node]` when `node` is set. |
| `open(mb=256, mkfs=MKFS, options="", log_mb=4*mb)` | Builds the stack, runs `mkfs` through it, marks `mkfs`, mounts with `options`; returns the mount point. |
| `mark(name)` | Logs a mark after every write flushed so far: a point `replay` can stop at. |
| `delay(ms)` | Every read, write and flush waits `ms` milliseconds (dm-delay), until `heal`. |
| `media(mode="error")` | Every write fails with `EIO` (`"error"`) or completes unwritten (`"drop"`), until `heal` (dm-flakey). |
| `heal()` | The fault layer passes I/O straight through again. |
| `stop()` | Unmounts, removes the devices (the target logs its end mark), reads the log; returns its entries. |
| `points()` | The entries replay can stop at: every flush, FUA write and mark. |
| `replay(to)` | Rebuilds a zeroed image up to entry `to` (an index or a mark name), mounts it so the filesystem recovers; returns the mount point. Writes `power_cut`. |
| `await close()`, `teardown()` | Unmounts everything, removes the devices and loops, deletes the directory. |
| `read_log(path)`, `find(entries, to)`, `rebuild(log, image, size, entries, to)` | The log reader and the replay, for a log kept elsewhere. |
| `sweep(prefix, everything=False, root=/var/tmp)` | Clears the labs of owners that died, or all with `prefix`. |

Fault events: `delay` (`ms`), `media` (`mode`), `heal`, `power_cut` (`at`, a mark or an entry
index), each with `nodes` when the lab has a node.

`python3 -I python/faultlab/disklab.py sweep [--prefix P] [--all] [--root DIR]` sweeps from the shell.
