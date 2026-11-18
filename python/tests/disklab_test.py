#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""disklab replays a log to any flush, FUA or mark, makes each fault it names, and leaves nothing behind.

Everywhere, a synthetic dm-log-writes log checks the reader and the replay, and a lab over a
recording runner checks the commands each call makes and that every fault writes its event into a
log history.py reads beside operations (A3). On Linux as root: a planted unsynced write is missing
at every replay point before its fsync and present at every one after, with seconds per replay
(A1); dm-delay adds its latency to fdatasync and dm-flakey fails or drops writes only inside its
window (A2); and teardown leaves nothing after close, exit, SIGTERM, SIGHUP, SIGINT and SIGKILL.
Elsewhere the live part prints SKIP with its reason; `--live` makes that a failure.

python3 -I tests/disklab_test.py [--live] [--root DIR]
"""

from __future__ import annotations

import argparse
import asyncio
import errno
import json
import mmap
import os
import shutil
import signal
import statistics
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from faultlab.disklab import (
    DISCARD,
    END,
    ENTRY,
    FLUSH,
    FUA,
    MAGIC,
    MARK,
    SUPER,
    Disklab,
    DisklabError,
    find,
    read_log,
    rebuild,
    sweep,
    unavailable,
)
from faultlab.history import Recorder, load_log
from faultlab.netlab import NetlabError, run

SCRATCH = Path(__file__).resolve().parents[2] / ".scratch" / "disklab_test"
A3_PREFIX = "faultlab-dk"
SECTOR = 512
O_DIRECT: int = getattr(os, "O_DIRECT", 0)  # the live probes run on Linux only
fdatasync: Callable[[int], None] = getattr(os, "fdatasync", os.fsync)

results: list[tuple[str, bool]] = []


def verdict(label: str, ok: bool) -> bool:
    results.append((label, ok))
    print(f"  {'ok:  ' if ok else 'FAIL:'} {label}", flush=True)
    return ok


class Recording:
    """A runner that records each command instead of running it."""

    def __init__(self) -> None:
        self.calls: list[str] = []

    def __call__(self, argv: Sequence[str], stdin: str | None = None, /) -> str:
        self.calls.append(" ".join(argv))
        return ""

    def take(self) -> list[str]:
        calls = self.calls[:]
        self.calls.clear()
        return calls


def synthetic(path: Path, entries: Sequence[tuple[int, bytes, int, str | None]]) -> None:
    """A dm-log-writes log of (sector, data, flags, mark) entries; a discard's data is its length in zeros."""
    with path.open("wb") as log:
        log.write(SUPER.pack(MAGIC, 1, len(entries), SECTOR).ljust(SECTOR, b"\0"))
        for sector, data, flags, mark in entries:
            name = (mark or "").encode()
            log.write((ENTRY.pack(sector, len(data) // SECTOR, flags, len(name)) + name).ljust(SECTOR, b"\0"))
            if not flags & DISCARD:
                log.write(data)


LOG = [
    (8, b"a" * 4096, 0, None),  # 0: an unflushed write
    (0, b"", FLUSH, None),  # 1
    (0, b"", MARK, "m1"),  # 2
    (16, b"b" * 4096, FUA, None),  # 3
    (8, bytes(4096), DISCARD, None),  # 4
    (24, b"c" * 1024, 0, None),  # 5
    (0, b"", MARK, END),  # 6
]


def replays() -> None:
    """The reader and the replay, on a synthetic log (A1's portable half)."""
    print("replay: a synthetic log", flush=True)
    log, image = SCRATCH / "synthetic.log", SCRATCH / "synthetic.img"
    synthetic(log, LOG)
    entries = read_log(log)
    verdict(
        "read_log reads every entry, its byte offset, size, flags and mark",
        [(e.offset, e.size, e.flags, e.mark) for e in entries] == [(s * SECTOR, len(d), f, m) for s, d, f, m in LOG],
    )
    verdict(
        "the replay points are the flush, the FUA write and the marks",
        [e.index for e in entries if e.point] == [1, 2, 3, 6],
    )

    def state(to: int | str) -> tuple[bytes, bytes, bytes]:
        rebuild(log, image, 1 << 16, entries, to)
        data = image.read_bytes()
        return data[4096:4100], data[8192:8196], data[12288:12292]

    verdict("to m1: the flushed write, nothing after", state("m1") == (b"aaaa", bytes(4), bytes(4)))
    verdict("to 3: the FUA write too", state(3) == (b"aaaa", b"bbbb", bytes(4)))
    verdict("to 4: the discard zeroes the first write", state(4) == (bytes(4), b"bbbb", bytes(4)))
    verdict(f"to {END}: everything", state(END) == (bytes(4), b"bbbb", b"cccc") and image.stat().st_size == 1 << 16)
    try:
        rebuild(log, image, 1 << 16, entries, "nowhere")
        missing = False
    except DisklabError:
        missing = True
    verdict("an unknown mark raises DisklabError", missing)


def events() -> None:
    """Every fault writes its event; history.py reads them between operations (A3)."""
    print("events: a recording lab", flush=True)
    path = SCRATCH / "events.jsonl"
    path.unlink(missing_ok=True)
    recorder = Recorder(path, scenario="faultlab/1 name=disklab seed=1")
    runner = Recording()
    root = SCRATCH / "root"
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    lab = Disklab(recorder, node=2, root=root, prefix="faultlab-unit", runner=runner)
    mnt = lab.open(64, options="commit=300")
    setup = runner.take()
    name = lab.name
    verdict(
        "open: two loops, log-writes under the fault layer, mkfs and mount through it, the mkfs mark",
        sum(c.startswith("losetup --find --show") for c in setup) == 2
        and f"dmsetup create {name}-log --table 0 131072 log-writes  " in setup
        and f"dmsetup create {name}-disk --table 0 131072 linear /dev/mapper/{name}-log 0" in setup
        and any(c.startswith("mkfs.ext4") and c.endswith(f"/dev/mapper/{name}-disk") for c in setup)
        and f"dmsetup message {name}-log 0 mark mkfs" in setup
        and setup[-1] == f"mount -o commit=300 /dev/mapper/{name}-disk {mnt}"
        and (lab.dir / "data.img").stat().st_size == 64 << 20,
    )

    expected: list[tuple[str, dict[str, object]]] = []
    commands: dict[str, list[str]] = {}

    def call(kind: str, args: dict[str, object], label: str, action: Callable[[], object]) -> None:
        recorder.record(f"c{len(expected)}", "write", "k", f"v{len(expected)}", "quorum", "invoke")
        action()
        commands[label] = runner.take()
        expected.append((kind, args | {"nodes": [2]}))
        recorder.record(f"c{len(expected) - 1}", "write", "k", f"v{len(expected) - 1}", "quorum", "ok")

    def swap(table: str) -> list[str]:
        disk = f"{name}-disk"
        return [f"dmsetup suspend --nolockfs {disk}", f"dmsetup load {disk} --table {table}", f"dmsetup resume {disk}"]

    call("delay", {"ms": 50}, "delay", lambda: lab.delay(50))
    call("media", {"mode": "error"}, "media error", lambda: lab.media("error"))
    call("media", {"mode": "drop"}, "media drop", lambda: lab.media("drop"))
    call("heal", {}, "heal", lab.heal)
    under = f"0 131072 {{}} /dev/mapper/{name}-log 0"
    verdict(
        "delay(50) swaps in dm-delay, without freezing the filesystem",
        commands["delay"] == swap(under.format("delay") + " 50"),
    )
    verdict(
        "media swaps in dm-flakey, down from the start, failing or dropping writes",
        commands["media error"] == swap(under.format("flakey") + " 0 1 1 error_writes")
        and commands["media drop"] == swap(under.format("flakey") + " 0 1 1 drop_writes"),
    )
    verdict("heal swaps back the linear table", commands["heal"] == swap(under.format("linear")))
    try:
        lab.media("corrupt")
        refused = False
    except ValueError:
        refused = True
    verdict("an unknown media mode raises ValueError and writes no event", refused and not runner.take())

    synthetic(lab.dir / "log.img", LOG)
    lab.stop()
    stopped = runner.take()
    verdict(
        "stop unmounts, removes the top device first, detaches the loops, and reads the log",
        stopped[0] == f"umount {mnt}"
        and stopped.index(f"dmsetup remove --retry {name}-disk") < stopped.index(f"dmsetup remove --retry {name}-log")
        and len(lab.entries) == len(LOG),
    )
    call("power_cut", {"at": "m1"}, "replay m1", lambda: lab.replay("m1"))
    call("power_cut", {"at": 3}, "replay 3", lambda: lab.replay(3))
    verdict(
        "replay mounts the rebuilt image over a loop, after unmounting the last one",
        commands["replay m1"][-1] == f"mount -o loop {lab.dir}/replay.img {lab.dir}/replay"
        and (lab.dir / "replay.img").read_bytes()[8192:8196] == b"bbbb",
    )
    lab.teardown()
    verdict("teardown deletes the lab's directory", not lab.dir.exists())
    recorder.close()

    log = load_log(path.read_text().splitlines())
    seen = [(fault.kind, fault.args) for fault in log.faults]
    verdict("each fault wrote its event, in order, with its arguments", seen == expected)
    verdict("history.py reads the operations between them", len(log.ops) == len(expected))
    times = [json.loads(line)["time"] for line in path.read_text().splitlines()[1:]]
    verdict("operations and faults share one clock", times == sorted(times))


# The live probes: Linux, root, in a VM.


def write(path: Path, data: bytes, sync: bool) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        os.write(fd, data)
        if sync:
            os.fsync(fd)
    finally:
        os.close(fd)


def holds(path: Path, data: bytes) -> bool:
    return path.is_file() and path.read_bytes() == data


def power_cut(root: Path) -> None:
    """A planted unsynced write is missing before its fsync's flush and present after it (A1)."""
    print("power cut: replay to every point around an fsync", flush=True)
    lab = Disklab(root=root, prefix=A3_PREFIX)
    try:
        mnt = lab.open(256)
        synced, planted = os.urandom(1 << 16), os.urandom(1 << 16)
        write(mnt / "synced", synced, sync=True)
        lab.mark("base")
        write(mnt / "planted", planted, sync=False)
        lab.mark("before")
        fd = os.open(mnt / "planted", os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
        lab.mark("after")
        entries = lab.stop()
        points = [e for e in lab.points() if find(entries, "before").index <= e.index <= find(entries, "after").index]
        seen: list[tuple[str, bool, bool]] = []
        seconds: list[float] = []
        for entry in points:
            start = time.perf_counter()
            state = lab.replay(entry.index)
            seconds.append(time.perf_counter() - start)
            seen.append((entry.describe(), holds(state / "planted", planted), holds(state / "synced", synced)))
        print("    " + "\n    ".join(f"{d}: planted {p}, synced {s}" for d, p, s in seen), flush=True)
        present = [p for _, p, _ in seen]
        flip = present.index(True) if True in present else len(present)
        verdict(
            f"planted is missing at the {flip} points from mark 'before' and present at the {len(present) - flip} "
            f"from {seen[flip][0] if flip < len(seen) else 'none'} to mark 'after'; synced at all",
            0 < flip < len(present) and not any(present[:flip]) and all(present[flip:]) and all(s for *_, s in seen),
        )
        size = (root / lab.name / "log.img").stat().st_blocks * 512
        verdict(
            f"seconds per replay (rebuild and recovering mount) over a {size / 2**20:.1f} MiB log of {len(entries)} "
            f"entries: median {statistics.median(seconds):.3f}, max {max(seconds):.3f}",
            max(seconds) < 30,
        )
    finally:
        lab.teardown()
    scale(root)


def scale(root: Path) -> None:
    """Seconds per replay against log size: a run that writes 256 MiB in 1 MiB fsynced files."""
    lab = Disklab(root=root, prefix=A3_PREFIX)
    try:
        mnt = lab.open(1024, log_mb=1024)
        for n in range(256):
            write(mnt / f"f{n}", os.urandom(1 << 20), sync=True)
        lab.mark("done")
        entries = lab.stop()
        size = (root / lab.name / "log.img").stat().st_blocks * 512
        timings: list[tuple[float, float]] = []
        for _ in range(3):
            start = time.perf_counter()
            rebuild(root / lab.name / "log.img", root / lab.name / "replay.img", lab.size, entries, "done")
            built = time.perf_counter()
            state = lab.replay("done")
            files = len(list(state.iterdir()))
            timings.append((built - start, time.perf_counter() - built))
        rebuilt = statistics.median(t for t, _ in timings)
        replayed = statistics.median(t for _, t in timings)
        verdict(
            f"a {size / 2**20:.0f} MiB log of {len(entries)} entries: rebuild {rebuilt:.2f} s "
            f"({size / 2**20 / rebuilt:.0f} MiB/s); replay, rebuild plus recovering mount, {replayed:.2f} s; "
            f"{files - 1} files",
            files - 1 == 256,  # lost+found
        )
    finally:
        lab.teardown()


def direct(path: Path, data: bytes, offset: int = 0) -> None:
    """One O_DIRECT write of `data` (page-aligned length) at `offset`."""
    buffer = mmap.mmap(-1, len(data))
    buffer.write(data)
    fd = os.open(path, os.O_WRONLY | O_DIRECT)
    try:
        os.pwrite(fd, buffer, offset)
    finally:
        os.close(fd)
        buffer.close()


def read_direct(path: Path, size: int, offset: int = 0) -> bytes:
    buffer = mmap.mmap(-1, size)
    fd = os.open(path, os.O_RDONLY | O_DIRECT)
    try:
        os.preadv(fd, [buffer], offset)
    finally:
        os.close(fd)
    data = bytes(buffer)
    buffer.close()
    return data


def fdatasyncs(path: Path, rounds: int) -> tuple[float, float]:
    """Median ms of fdatasync after an O_DIRECT overwrite (one flush), and of a buffered write plus fsync."""
    flush: list[float] = []
    full: list[float] = []
    for n in range(rounds):
        direct(path, bytes([n]) * 4096)
        fd = os.open(path, os.O_WRONLY)
        start = time.perf_counter()
        fdatasync(fd)
        flush.append((time.perf_counter() - start) * 1000)
        start = time.perf_counter()
        os.pwrite(fd, bytes([n]) * 4096, 8192)
        os.fsync(fd)
        full.append((time.perf_counter() - start) * 1000)
        os.close(fd)
    return statistics.median(flush), statistics.median(full)


def delay_and_media(root: Path) -> None:
    """dm-delay adds its latency to fdatasync; dm-flakey fails or drops writes only in its window (A2)."""
    print("delay and media: probes on the lab's filesystem", flush=True)
    lab = Disklab(root=root, prefix=A3_PREFIX)
    try:
        mnt = lab.open(128, options="commit=300")  # no periodic journal commit inside a media window
        file = mnt / "probe"
        write(file, bytes(1 << 20), sync=True)
        base, base_full = fdatasyncs(file, 15)
        lab.delay(100)
        slow, slow_full = fdatasyncs(file, 15)
        lab.heal()
        healed, _ = fdatasyncs(file, 15)
        added = slow - base
        verdict(
            f"delay(100) adds {added:.1f} ms to fdatasync (median {base:.2f} -> {slow:.2f}; healed {healed:.2f}); "
            f"a buffered write plus fsync gains {slow_full - base_full:.0f} ms ({(slow_full - base_full) / 100:.1f} trips)",
            90 <= added <= 110 and healed - base < 10,
        )

        def attempts(rounds: int, tag: int) -> tuple[int, int]:
            """Writes `rounds` 4 KiB blocks; returns how many failed with EIO and how many read back."""
            failed = landed = 0
            for n in range(rounds):
                data = bytes([tag]) * 4096
                try:
                    direct(file, data, n * 4096)
                except OSError as error:
                    failed += error.errno == errno.EIO
                    continue
                landed += read_direct(file, 4096, n * 4096) == data
            return failed, landed

        for mode, tag in (("error", 1), ("drop", 4)):
            before = attempts(20, tag)
            lab.media(mode)
            inside = attempts(20, tag + 1)
            lab.heal()
            after = attempts(20, tag + 2)
            want = (20, 0) if mode == "error" else (0, 0)
            verdict(
                f"media({mode!r}): (EIO, landed) of 20 writes before {before}, inside {inside}, after {after}",
                before == (0, 20) and inside == want and after == (0, 20),
            )
        lab.stop()
    finally:
        lab.teardown()


# Teardown: a child builds a lab and ends one way; nothing of it may stay.


def ours(root: Path) -> list[str]:
    """Every device, loop, mount and directory of an A3_PREFIX lab."""
    found = [line.split()[0] for line in run(["dmsetup", "ls"]).splitlines() if line.startswith(A3_PREFIX + "-")]
    found += [
        line
        for line in run(["losetup", "--list", "--noheadings", "--output", "BACK-FILE"]).splitlines()
        if f"/{A3_PREFIX}-" in line
    ]
    found += [
        line.split()[1] for line in Path("/proc/self/mounts").read_text().splitlines() if f"/{A3_PREFIX}-" in line
    ]
    return found + [str(p) for p in root.iterdir() if p.name.startswith(A3_PREFIX + "-")]


def listing(root: Path) -> str:
    devices = run(["dmsetup", "ls"]).strip()
    loops = run(["losetup", "--list", "--noheadings", "--output", "NAME,BACK-FILE"]).strip()
    mounts = sorted(line.split()[1] for line in Path("/proc/self/mounts").read_text().splitlines())
    return f"devices: {devices}\nloops: {loops}\nmounts: {' '.join(mounts)}\nroot: {' '.join(sorted(os.listdir(root)))}"


def child(end: str, root: Path) -> int:
    lab = Disklab(root=root, prefix=A3_PREFIX)
    mnt = lab.open(64)
    write(mnt / "file", b"x" * 4096, sync=True)
    if end.endswith("replay"):  # ends while a replay is mounted
        lab.stop()
        lab.replay("mkfs")
    else:
        lab.delay(1)
    print(f"ready {len(ours(root))} pieces", flush=True)
    try:
        sys.stdin.readline()  # the parent has counted; a signal may come instead
    except KeyboardInterrupt:
        return 130
    if end == "close":
        asyncio.run(lab.close())
    return 0


def teardown(root: Path) -> None:
    print("teardown: a child lab, ended each way", flush=True)
    before = listing(root)
    print("  before:\n    " + before.replace("\n", "\n    "), flush=True)
    script = str(Path(__file__).resolve().parents[1] / "faultlab" / "disklab.py")
    for end in ("close", "exit", "SIGTERM", "SIGHUP", "SIGINT", "SIGKILL", "SIGKILL replay"):
        proc = subprocess.Popen(
            [sys.executable, "-I", __file__, "child", end, "--root", str(root)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        assert proc.stdin is not None and proc.stdout is not None
        ready = proc.stdout.readline().strip()
        built = len(ours(root))
        if end.startswith("SIG"):
            proc.send_signal(getattr(signal, end.split()[0]))
        else:
            proc.stdin.write("go\n")
            proc.stdin.flush()
        proc.wait(60)
        left = ours(root)
        if end.startswith("SIGKILL"):
            swept = subprocess.run(
                [sys.executable, "-I", script, "sweep", "--prefix", A3_PREFIX, "--root", str(root)],
                capture_output=True,
                text=True,
                check=False,
            ).stdout.strip()
            verdict(
                f"{end}: {ready}; leaves {len(left)} of {built}, which sweep clears ({swept}); none left: {not ours(root)}",
                built > 0 and len(left) == built and not ours(root),
            )
        else:
            verdict(f"{end}: {ready}; {built} built, {len(left)} left", built > 0 and not left)
    after = listing(root)
    print("  after:\n    " + after.replace("\n", "\n    "), flush=True)
    verdict("the listing after equals the listing before", after == before)
    proc = subprocess.Popen(
        [sys.executable, "-I", __file__, "child", "SIGKILL", "--root", str(root)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    assert proc.stdout is not None
    proc.stdout.readline()
    stale = len(ours(root))
    proc.kill()
    proc.wait(30)
    lab = Disklab(root=root, prefix=A3_PREFIX)
    lab.open(32)
    mine = ours(root)
    lab.teardown()
    verdict(
        f"a new lab clears the {stale} pieces of a killed owner, keeping only its own {len(mine)}",
        stale > 0 and len(mine) > 0 and all(lab.name in piece for piece in mine) and not ours(root),
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="disklab_test.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("--live", action="store_true", help="fail, rather than skip, where disklab cannot run")
    parser.add_argument("--root", type=Path, default=Path("/dev/shm"), help="where live labs keep their images")
    if argv[:1] == ["child"]:
        return child(argv[1], parser.parse_args(argv[2:]).root)
    args = parser.parse_args(argv)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    replays()
    events()
    if (reason := unavailable()) is None:
        try:
            power_cut(args.root)
            delay_and_media(args.root)
            teardown(args.root)
        except NetlabError as error:
            verdict(f"disklab: {error}", False)
        finally:
            sweep(A3_PREFIX, everything=True, root=args.root)
    elif args.live:
        verdict(f"live probes: {reason}", False)
    else:
        print(f"SKIP live probes and teardown: {reason}", flush=True)
    failures = [label for label, ok in results if not ok]
    print(f"=== VERDICT: {'GREEN' if not failures else f'RED ({len(failures)} failures)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
