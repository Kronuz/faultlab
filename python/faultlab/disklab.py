#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""The disk tier: a filesystem on device-mapper, every write logged with its flush and FUA marks.

A lab is a directory `<root>/<prefix>-<pid>-<n>` holding two sparse images, each on a loop device,
and two device-mapper devices stacked on them, with a filesystem on top:

    filesystem      mounted at <dir>/mnt
    <name>-disk     linear, or dm-delay, or dm-flakey: the fault layer, swapped by a table reload
    <name>-log      dm-log-writes: data.img gets each write; log.img records it, in completion order
    loop, loop      data.img, log.img

mkfs runs through the stack, so the log holds every write since a zeroed device: after `stop`,
`replay(to)` rebuilds a zeroed image from the log up to any flush, FUA or mark entry and mounts it,
which lets the filesystem recover from exactly the state a power cut at that point leaves. Each
fault call (delay, media, heal, replay's power cut) writes its event when the lab has a log.

Needs Linux, root, and the dm-log-writes, dm-delay and dm-flakey targets: run it in a VM. Teardown
unmounts, removes the devices, detaches the loops and deletes the directory; it runs at close, at
exit and on SIGTERM or SIGHUP, and a new Disklab sweeps the labs of owners that died (SIGKILL).

python3 -I faultlab/disklab.py sweep [--prefix faultlab] [--all]
"""

from __future__ import annotations

import argparse
import atexit
import contextlib
import os
import re
import shutil
import signal
import struct
import sys
import threading
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from types import FrameType

if __package__ in (None, ""):  # run as a script: python3 -I faultlab/disklab.py
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from faultlab.netlab import FaultLog, NetlabError, Runner, run

PREFIX = "faultlab"
ROOT = Path("/var/tmp")
MKFS = ("mkfs.ext4", "-q", "-E", "nodiscard,lazy_itable_init=0,lazy_journal_init=0")  # no background zeroing
TERMINATING = (signal.SIGTERM, signal.SIGHUP)  # their default action skips atexit

# dm-log-writes' on-disk format (drivers/md/dm-log-writes.c): a super block in the first sector,
# then per entry one sector holding its header (and a mark's name), followed by its data sectors.
MAGIC = 0x6A736677736872
SUPER = struct.Struct("<QQQI")  # magic, version, nr_entries, sectorsize
ENTRY = struct.Struct("<QQQQ")  # sector, nr_sectors, flags, data_len; sectors in sectorsize units
FLUSH, FUA, DISCARD, MARK = 1, 2, 4, 8
END = "dm-log-writes-end"  # the mark the target writes when it is removed


class DisklabError(NetlabError):
    """A command disklab ran failed, the host cannot run disklab, or a log is unreadable."""


def unavailable() -> str | None:
    """Why this host cannot run disklab, or None when it can."""
    if sys.platform != "linux":
        return f"disklab needs Linux device-mapper, not {sys.platform}"
    if os.geteuid() != 0:
        return "disklab needs root (losetup, dmsetup, mount)"
    return None


@dataclass(frozen=True)
class Entry:
    """One logged write, discard, flush or mark; `offset` and `size` in bytes on the device."""

    index: int
    offset: int
    size: int
    flags: int
    mark: str | None
    data: int  # where its bytes start in the log

    @property
    def point(self) -> bool:
        """A replay point: a flush, a FUA write, or a mark."""
        return bool(self.flags & (FLUSH | FUA | MARK))

    def describe(self) -> str:
        kinds = [name for bit, name in ((FLUSH, "flush"), (FUA, "fua"), (DISCARD, "discard")) if self.flags & bit]
        if self.mark is not None:
            return f"#{self.index} mark {self.mark}"
        return f"#{self.index} {'+'.join(kinds or ['write'])} {self.size}B at {self.offset}"


def read_log(path: Path) -> list[Entry]:
    """Every entry of a dm-log-writes log, in order; raises DisklabError when it is not one."""
    with path.open("rb") as log:
        magic, version, count, sectorsize = SUPER.unpack(log.read(SUPER.size))
        if magic != MAGIC or version != 1 or sectorsize < 512:
            raise DisklabError(f"{path}: not a dm-log-writes log (magic {magic:#x}, version {version})")
        entries: list[Entry] = []
        at = sectorsize
        for index in range(count):
            log.seek(at)
            header = log.read(sectorsize)
            if len(header) < ENTRY.size:
                raise DisklabError(f"{path}: entry {index} of {count} is past the end of the log")
            sector, sectors, flags, length = ENTRY.unpack_from(header)
            mark = header[ENTRY.size : ENTRY.size + length].decode() if flags & MARK else None
            at += sectorsize
            entries.append(Entry(index, sector * sectorsize, sectors * sectorsize, flags, mark, at))
            if not flags & DISCARD:
                at += sectors * sectorsize
    return entries


def find(entries: Sequence[Entry], to: int | str) -> Entry:
    """The entry with index `to`, or the mark named `to`."""
    for entry in entries:
        if entry.index == to if isinstance(to, int) else entry.mark == to:
            return entry
    raise DisklabError(f"the log has no {'entry' if isinstance(to, int) else 'mark'} {to!r}")


def rebuild(log: Path, image: Path, size: int, entries: Sequence[Entry], to: int | str) -> Entry:
    """Writes a zeroed `size`-byte `image` holding the log's entries up to `to`, inclusive; returns it."""
    last = find(entries, to)
    chunk = 1 << 20
    with log.open("rb") as source, image.open("wb") as target:
        target.truncate(size)
        for entry in entries[: last.index + 1]:
            if entry.flags & DISCARD:
                for at in range(entry.offset, entry.offset + entry.size, chunk):
                    os.pwrite(target.fileno(), bytes(min(chunk, entry.offset + entry.size - at)), at)
                continue
            for done in range(0, entry.size, chunk):
                block = os.pread(source.fileno(), min(chunk, entry.size - done), entry.data + done)
                os.pwrite(target.fileno(), block, entry.offset + done)
    return last


_live: set[Disklab] = set()
_trapped = False


def _teardown_all() -> None:
    for lab in tuple(_live):
        lab.teardown()


def _terminated(signum: int, frame: FrameType | None) -> None:
    try:
        _teardown_all()
    finally:
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)


def _trap() -> None:
    """Once per process: tears every live lab down at exit, and on a terminating signal left at its default."""
    global _trapped
    if _trapped:
        return
    _trapped = True
    atexit.register(_teardown_all)
    if threading.current_thread() is threading.main_thread():
        for signum in TERMINATING:
            if signal.getsignal(signum) == signal.SIG_DFL:
                signal.signal(signum, _terminated)


_instances = 0


class Disklab:
    """One filesystem over a logged, faultable device; `node` names it in fault events."""

    def __init__(
        self,
        log: FaultLog | None = None,
        node: int | None = None,
        root: Path = ROOT,
        prefix: str = PREFIX,
        runner: Runner = run,
    ) -> None:
        global _instances
        _instances += 1
        self.log = log
        self.node = node
        self.prefix = prefix
        self.run = runner
        self.name = f"{prefix}-{os.getpid()}-{_instances}"
        self.dir = root / self.name
        self.mnt = self.dir / "mnt"
        self.size = 0
        self.options = ""
        self.mounted = False  # the lab's filesystem; False after stop
        self.layer = "linear"
        self.entries: list[Entry] = []

    def _fault(self, kind: str, **args: object) -> None:
        if self.log is not None:
            self.log.fault(kind, **args, **({"nodes": [self.node]} if self.node is not None else {}))

    def device(self, layer: str = "disk") -> str:
        return f"/dev/mapper/{self.name}-{layer}"

    def open(self, mb: int = 256, mkfs: Sequence[str] = MKFS, options: str = "", log_mb: int = 0) -> Path:
        """Builds the stack on a `mb`-MiB device with a `log_mb`-MiB log (4 * mb by default), makes the
        filesystem and mounts it with `options`; returns the mount point."""
        if self.size:
            raise DisklabError(f"{self.name} is already open")
        if mb < 16:
            raise ValueError(f"a {mb} MiB device is too small for a filesystem")
        sweep(self.prefix, self.run, root=self.dir.parent)
        _trap()
        _live.add(self)
        self.size = mb << 20
        self.options = options
        try:
            self.mnt.mkdir(parents=True)
            loops: list[str] = []
            for image, size in (("data.img", self.size), ("log.img", (log_mb or 4 * mb) << 20)):
                with (self.dir / image).open("wb") as file:
                    file.truncate(size)  # sparse: only what is written takes space
                loops.append(self.run(["losetup", "--find", "--show", str(self.dir / image)]).strip())
            sectors = self.size >> 9
            self.run(["dmsetup", "create", f"{self.name}-log", "--table", f"0 {sectors} log-writes {' '.join(loops)}"])
            self.run(["dmsetup", "create", f"{self.name}-disk", "--table", self._table("linear")])
            self.run([*mkfs, self.device()])
            self.mark("mkfs")
            self.run(["mount", *(["-o", options] if options else []), self.device(), str(self.mnt)])
            self.mounted = True
        except BaseException:
            with contextlib.suppress(NetlabError):
                self.teardown()
            raise
        return self.mnt

    def _table(self, target: str, *args: str) -> str:
        return " ".join(("0", str(self.size >> 9), target, self.device("log"), "0", *args))

    def _load(self, table: str) -> None:
        if not self.mounted:
            raise DisklabError(f"{self.name} is not running")
        disk = f"{self.name}-disk"
        self.run(["dmsetup", "suspend", "--nolockfs", disk])  # waits for in-flight I/O; no freeze, so no sync
        self.run(["dmsetup", "load", disk, "--table", table])
        self.run(["dmsetup", "resume", disk])

    def mark(self, name: str) -> None:
        """Logs a mark after every write completed so far: a point `replay` can stop at."""
        if not re.fullmatch(r"[\w.:-]{1,64}", name) or name == END:
            raise ValueError(f"mark {name!r}: 1 to 64 letters, digits, '_', '.', ':' or '-'")
        self.run(["dmsetup", "message", f"{self.name}-log", "0", "mark", name])

    def delay(self, ms: int) -> None:
        """Delays every read, write and flush by `ms` milliseconds (dm-delay); heal lifts it."""
        if ms < 0:
            raise ValueError(f"delay {ms} is negative")
        self._load(self._table("delay", str(ms)))
        self.layer = f"delay {ms}"
        self._fault("delay", ms=ms)

    def media(self, mode: str = "error") -> None:
        """Fails every write with EIO ("error") or completes it without writing ("drop") until heal (dm-flakey)."""
        if mode not in ("error", "drop"):
            raise ValueError(f"media mode {mode!r} is not error or drop")
        self._load(self._table("flakey", "0", "1", "1", f"{mode}_writes"))  # up 0 s, down 1 s: always down
        self.layer = f"media {mode}"
        self._fault("media", mode=mode)

    def heal(self) -> None:
        """Ends the delay or media fault: the device passes I/O straight through again."""
        self._load(self._table("linear"))
        self.layer = "linear"
        self._fault("heal")

    def stop(self) -> list[Entry]:
        """Unmounts, removes the stack, which ends the log, and reads it; returns its entries."""
        if not self.mounted:
            raise DisklabError(f"{self.name} is not running")
        self.mounted = False
        self.run(["umount", str(self.mnt)])
        _remove(self.run, [f"{self.name}-disk", f"{self.name}-log"])
        _detach(self.run, self.name, ("data.img", "log.img"))
        self.entries = read_log(self.dir / "log.img")
        if not self.entries or self.entries[-1].mark != END:
            raise DisklabError(f"{self.name}: the log ends before {END}: it was full or logging failed")
        return self.entries

    def points(self) -> list[Entry]:
        """The entries replay can stop at: every flush, FUA write and mark, in order."""
        return [entry for entry in self.entries if entry.point]

    def replay(self, to: int | str) -> Path:
        """Mounts the state a power cut leaves just after entry or mark `to`, recovered; returns its mount point."""
        if not self.entries:
            raise DisklabError(f"{self.name}: replay needs a stopped lab")
        self.unmount_replay()
        image = self.dir / "replay.img"
        entry = rebuild(self.dir / "log.img", image, self.size, self.entries, to)
        target = self.dir / "replay"
        target.mkdir(exist_ok=True)
        self._fault("power_cut", at=entry.mark if entry.mark is not None else entry.index)
        self.run(["mount", "-o", "loop", str(image), str(target)])
        return target

    def unmount_replay(self) -> None:
        if (self.dir / "replay").is_dir():
            _clear(self.run, self.name, only_replay=True)

    def teardown(self) -> None:
        """Unmounts everything, removes the devices and loops, deletes the directory; idempotent."""
        _live.discard(self)
        self.mounted = False
        self.entries = []
        if self.size:
            self.size = 0
            _clear(self.run, self.name, self.dir)

    async def close(self) -> None:
        self.teardown()


def _remove(runner: Runner, devices: Sequence[str]) -> None:
    for device in devices:
        with contextlib.suppress(NetlabError):
            runner(["dmsetup", "resume", device])  # a device suspended mid-reload cannot be removed
        runner(["dmsetup", "remove", "--retry", device])


def _detach(runner: Runner, name: str, images: Sequence[str] = ()) -> set[Path]:
    """Detaches lab `name`'s loops (or those of `images`); returns the directories their files are in."""
    homes: set[Path] = set()
    for line in runner(["losetup", "--list", "--noheadings", "--output", "NAME,BACK-FILE"]).splitlines():
        loop, _, back = line.partition(" ")
        file = Path(back.strip().removesuffix(" (deleted)"))
        if file.parent.name == name and (not images or file.name in images):
            runner(["losetup", "--detach", loop])
            homes.add(file.parent)
    return homes


def _clear(runner: Runner, name: str, home: Path | None = None, only_replay: bool = False) -> None:
    """Undoes lab `name`, top down: its mounts, devices, loops and directory (or only its replay mount)."""
    ours = [Path(target) for target in _mounts(runner) if Path(target).parent.name == name]  # <dir>/mnt, <dir>/replay
    for target in reversed(ours):  # the latest first; a loop mount detaches its loop
        if not only_replay or target.name == "replay":
            runner(["umount", str(target)])
    if only_replay:
        return
    devices = [line.split()[0] for line in runner(["dmsetup", "ls"]).splitlines() if line.startswith(f"{name}-")]
    homes = {target.parent for target in ours} | ({home} if home is not None else set())
    _remove(runner, sorted(devices, key=lambda device: not device.endswith("-disk")))  # the top layer first
    homes |= _detach(runner, name)
    for directory in homes:
        if directory.is_dir():
            if any(Path(target).is_relative_to(directory) for target in _mounts(runner)):
                raise DisklabError(f"{directory} still holds a mount; not deleting it")
            shutil.rmtree(directory)


def _mounts(runner: Runner) -> list[str]:
    return [line.split()[1] for line in runner(["cat", "/proc/self/mounts"]).splitlines() if len(line.split()) > 1]


def sweep(prefix: str = PREFIX, runner: Runner = run, everything: bool = False, root: Path = ROOT) -> list[str]:
    """Clears the labs named with `prefix` whose owner died (or all of them); returns their names."""
    owned = re.compile(rf"(?:^|/)({re.escape(prefix)}-(\d+)-\d+)(?:-disk|-log|/|$)")
    found: dict[str, Path] = {}
    lines = runner(["dmsetup", "ls"]).splitlines()
    lines += runner(["losetup", "--list", "--noheadings", "--output", "BACK-FILE"]).splitlines()
    lines += [str(path) for path in root.iterdir()] if root.is_dir() else []
    lines += _mounts(runner)
    for line in lines:
        if (match := owned.search(line)) and (everything or not _alive(int(match[2]))):
            found.setdefault(match[1], root / match[1])
    for name, home in sorted(found.items()):
        _clear(runner, name, home)
    return sorted(found)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="disklab.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    clean = sub.add_parser("sweep", help="clear the labs whose owner died")
    clean.add_argument("--prefix", default=PREFIX)
    clean.add_argument("--all", action="store_true", help="also those of live owners")
    clean.add_argument("--root", type=Path, default=ROOT, help="where the labs' directories are")
    args = parser.parse_args(argv)
    if (reason := unavailable()) is not None:
        print(f"disklab.py: {reason}", file=sys.stderr)
        return 2
    removed = sweep(args.prefix, everything=args.all, root=args.root)
    print(f"swept {len(removed)} lab(s){': ' if removed else ''}{' '.join(removed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
