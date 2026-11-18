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
| `power_off_after(n)` / `powered_off()` | After `n` more mutations, each one fails with `EIO` before its effect, until `crash()`. |
| `mutations()`, `syncs()`, `calls()` | Counters: mutating calls (crash points), syncs that reached the device, every IO and File call. |
| `add_fault(fault)` / `clear_faults()` | Fails the `nth` call of `fault.kind` with `fault.error`, before or `after` its effect; a sync with `keep` loses its pages (Linux writeback-error semantics). |
| `add_media_fault(path, media, begin, end, times = 0)` / `media_hits()` | Reads overlapping `[begin, end)` fail (`read_error`) or flip a bit (`bit_flip`). |
| `short_writes(max_bytes)` | Each write stores at most `max_bytes` and returns the count. |
| `damage_writes(min, at, count = 1)` / `damaged_writes()` | Silent damage: a flipped bit `at` bytes into each of the next `count` writes of at least `min` bytes. |
| `set_capacity(bytes)` / `used()` | `ENOSPC` past the capacity; `used` counts bytes and preallocated extents. |
| `classify(FilesystemClass)`, `set_trust(DirectoryTrust)`, `map_files(bool)` | What `io` reports for `directory_filesystem`, `directory_trust`, and whether `File::map` maps. |
| `view(path) const` | The current bytes of a file, valid until the next change to it. |
| `list(directory = {}) const` | The names in a directory, files and subdirectories, sorted. |

### `crash_policy`

Each field fixes one family of coins; `coin` (the default) flips it. A fixed pick still draws and
discards its coin, so the other families see the stream they always did.

| Field | Picks | Fixed meaning |
|---|---|---|
| `epochs` | `coin`, `none`, `all` | Ordering points kept whole: none, or all of them. |
| `names` | `coin`, `durable`, `all`, `additions` | Namespace ops the epoch cut admits: none, all, or only those that add a name. |
| `size` | `coin`, `durable`, `current`, `sector` | Each dirty file's size: the old one, the new one, or any sector boundary between (one draw). |
| `sectors` | `coin`, `old`, `fresh` | Each unsynced 512-byte sector's bytes: old, or new. |

`size_pick` changes how many sector coins follow (their count depends on the size), so a size
pick moves the sector bytes the seed draws; `sector_pick` and `name_pick` leave sizes alone.

### `io` and `file`

`io(filesystem&, directory = {})` is a `journal::IO` over one directory of the model (`ENOENT`
when it is missing); its files are `journal::File`s. The whole seam is modeled:

- Opens, `create_exclusive`, `replace` (atomic rename), `link` (never replaces: `EEXIST`),
  `remove`, scans, footprints and `inspect_entry` (device 1, the effective uid, mode 0600, 0700
  for a directory). A directory refuses `remove` and `link` (`EPERM`) and `open` (`EISDIR`).
- `sync` and `sync_directory`; the group syncs (`sync_files`, `sync_data_files`,
  `sync_files_and_directory`) are the seam's defaults over them; `order_files_and_directory` is
  the ordering point that stages each file ahead of later changes.
- `preallocate` (a mutation; reserves space), `map` (read-only, with `map_files`),
  `map_shared` (`EACCES` read-only, `EINVAL` past the end; its stores reach the model when a call
  folds them), `try_lease` (shared leases coexist, an exclusive one excludes), `inspect`.
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
