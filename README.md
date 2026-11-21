# faultlab

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/faultlab/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/faultlab/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)
![Python 3.12](https://img.shields.io/badge/python-3.12-blue.svg)

One fault-injection library for storage and cluster code. faultfs is an in-memory filesystem
behind [completion](https://github.com/Kronuz/completion)'s `journal::IO` seam that rebuilds,
from a seed, any state a power loss could leave; a recording IO replays a real program's calls
into it; one scenario line replays a failing case; one event log carries a run's operations and
faults to one history checker. The C++ half is header-only and needs only completion's `io.h`;
the Python half is stdlib only.

```
C++, header-only, needs completion/io.h only       Python, stdlib only, python3 -I
  faultlab/faultfs.h    crash and media model         faultlab/history.py   the one checker
  faultlab/recorder.h   recording IO decorator        faultlab/proxy.py     portable network tier
                                                      faultlab/disklab.py   Linux dm-log-writes, dm-delay, dm-flakey
                                                      faultlab/netlab.py    Linux netns, netem, nftables
  faultlab/scenario.h   seeded scenario line          faultlab/scenario.py  the same line
  faultlab/events.h     JSON-line event writer
                         \___ one scenario line, one event log, read by the one checker ___/
```

## ✨ Highlights

- **The IO contract's crash states, from a seed.** Each unsynced 512-byte sector keeps its old
  bytes or its new ones; each unsynced create, link, rename and unlink survives or not, in order,
  a rename atomically; ordering points (`order_files_and_directory`) keep whole epochs. The same
  seed always rebuilds the same state.
- **Fixed picks beside the coins.** `crash_policy` forces a field's coins (every sector old, every
  name durable, every epoch kept) and `size_pick::sector` cuts an unsynced append at any sector
  boundary, the states journal's MemoryIO and storage's CrashIO reach.
- **Process death, snapshots and power cuts.** `kill()` ends a process and keeps what the device
  holds; `snapshot()` copies the model to sweep cut points from one prefix; `power_off_after(n)`
  fails every mutation after the n-th, `powered_off()` says so, and `power_on()` ends the cut as a
  process crash instead of a power loss.
- **The whole seam.** Directories on one device, `link`, `preallocate`, `map_shared`,
  `try_lease`, inspection and every owner-lock mode, with fault rules, media faults, short and
  damaged writes and a capacity.
- **Record a real run, crash it anywhere.** `recorder` runs every call on a real IO and on a
  shadow faultfs, snapshots the shadow after each mutating call, and `verify()` fails when the
  program changed the directory outside the seam.
- **One replay line, one log, one checker.** `faultlab/1 name=... seed=...` replays a case through
  `FAULTLAB_SCENARIO`; the JSON-line log holds operations, faults and harness records; the checker
  proves written, prefix, linearizable, monotonic and seal histories and lists the faults inside a
  violation.

## 🚀 Quick start

Publish a file through the seam, cut the power after each of its calls, crash with a few seeds,
and check that recovery sees the old version or the new one ([examples/demo.cc](examples/demo.cc)):

```cpp
#include "faultlab/faultfs.h"
#include "faultlab/scenario.h"

namespace ff = kronuz::faultlab;

ff::filesystem base;
{
	ff::io io(base);
	publish(io, "doc", old_bytes);         // your code, through journal::IO
}
for (std::uint64_t cut = 0; cut <= steps; ++cut) {
	for (std::uint64_t seed = 0; seed < 32; ++seed) {
		ff::scenario s{"demo.publish", seed, cut, std::nullopt, {}};
		if (!s.selected()) { continue; }   // FAULTLAB_SCENARIO runs one case
		auto fs = base.snapshot();
		fs.power_off_after(cut);
		try {
			ff::io io(fs);
			publish(io, "doc", new_bytes);
		} catch (const std::system_error&) {}  // the power went off
		fs.crash(seed);                     // or fs.crash(seed, {.sectors = ff::sector_pick::old})
		if (!recovers(fs)) { std::printf("replay with %s\n", s.replay().c_str()); }
	}
}
```

On Linux, the labs put real kernel faults under a cluster. netlab gives each node its own network
namespace, with netem on every link and nftables cuts; disklab puts device-mapper targets under
each node's directory. Python only issues `ip`, `tc`, `nft` and `dmsetup` commands and logs each
fault, so packets and blocks never pass through it. Both need root, so run them in a Linux VM:

```python
from faultlab.netlab import Netlab
lab = Netlab(log)                    # writes each fault to the event log
lab.open({1: 8890, 2: 8890, 3: 8890})
lab.delay(20); lab.loss(0.01)        # every link: 20 ms, 1% loss
lab.partition(1, 2); lab.heal()
```

A cluster harness writes the event log and checks it:

```sh
python3 -I faultlab/history.py run/events.jsonl
#   violation: k: linearizable: ...; faults inside: partition nodes=['n1', 'n2'] at 1.500 (line 5)
#   replay: FAULTLAB_SCENARIO='faultlab/1 name=events seed=3'
```

## 📦 Installation

Requires a C++20 compiler and CMake 3.18 or newer; it fetches completion at a pinned commit.
The tests need Python 3.12 and fetch nothing else; the MemoryIO states they check are recorded in
`test/fixtures/memory_io_states.txt` (regenerate with `-DFAULTLAB_REGENERATE_MEMORY_IO=ON`, which fetches journal). CI builds it on Linux (x86_64,
arm64), macOS (arm64, x86_64) and FreeBSD, plus ASan, UBSan and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(faultlab
  GIT_REPOSITORY https://github.com/Kronuz/faultlab.git
  GIT_TAG        <commit>)
FetchContent_MakeAvailable(faultlab)

target_link_libraries(my_tests PRIVATE faultlab::faultlab)
```

The Python modules are plain files: run `python3 -I <checkout>/python/faultlab/history.py`, or
put `<checkout>/python` on `sys.path` and `from faultlab import disklab, history, netlab, proxy, scenario`.

## 🎯 When to use it

Crash and fault tests of code that does its I/O through `journal::IO`; a cluster run on a lossy,
slow or partitioned network (netlab) or a slow, failing or reordering disk (disklab); and
histories of replicated registers under those faults.

Not for:

- **Throughput or latency numbers through the proxy.** `proxy.py` is the portable network tier
  (macOS has no network namespaces): bytes cross a Python process, so it injects faults but
  colors timings. Measure through netlab, where the kernel carries the packets.

- **Proving what a real filesystem does.** faultfs models the IO contract (an independent coin
  per sector and per namespace operation), which allows more than ext4, XFS or APFS leave; the
  mapping from the contract to fsync, `F_FULLFSYNC` and barriers is completion's to prove.
- **Weakened syncs.** faultfs models durable syncs only; a process that weakens them is not
  crash-tested.
- **Concurrent stores into a shared mapping while another thread calls in.** A fold reads the
  mapping without synchronization, as the page cache would.

## 📚 Documentation

- [docs/api.md](docs/api.md): every member a test touches, C++ and Python.
- [docs/mechanisms/netlab.md](docs/mechanisms/netlab.md) and
  [docs/mechanisms/disklab.md](docs/mechanisms/disklab.md): the Linux labs' topology, faults and
  teardown.
- [docs/mechanisms/crash-model.md](docs/mechanisms/crash-model.md): how `crash` rebuilds a state,
  the policies, the link rule, shared-mapping folds and the recorder.
- [Design](https://github.com/Kronuz/cpp-libraries/blob/main/docs/design/faultlab.md) (F1-F16)
  and the [catalog](https://github.com/Kronuz/cpp-libraries) in Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

`test/strata_seeds.inc` records the states strata's faultfs rebuilt for 384 seeds;
`test/tools/strata_seeds.sh` regenerates it from a strata checkout.

## 🤝 Contributing

Issues and pull requests are welcome. A change to `crash` must keep every recorded seed's state
(`faultlab_seeds`), or say why it moves them. Read the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright © 2026 Germán Méndez Bravo (Kronuz). See [LICENSE](LICENSE).
