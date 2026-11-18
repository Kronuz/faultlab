#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""The Linux network tier: a network namespace per node, joined by veth pairs, with netem and nftables.

Each pair of nodes (a, b) gets one veth pair: in a's namespace the end `to<b>`, at 10.<lo>.<hi>.<a>/24,
and in b's the end `to<a>`, at 10.<lo>.<hi>.<b>, where lo and hi are the pair sorted. Each end's egress
carries a netem qdisc, so each direction of each link has its own delay, loss, reorder, duplicate and
rate; each namespace's nftables table drops what arrives from the nodes cut off from it, one set element
per directed pair. Netlab has Mesh's interface (partition, heal, isolate, reset, cap) plus delay, loss,
reorder and duplicate, and each call writes its fault event when it has a log.

Needs Linux and root. Teardown deletes the namespaces, which takes their veths, qdiscs and tables with
them; it runs at close, at exit and on SIGTERM or SIGHUP, and a new Netlab sweeps the namespaces of
owners that died (SIGKILL).

python3 -I faultlab/netlab.py sweep [--prefix faultlab] [--all]
"""

from __future__ import annotations

import argparse
import atexit
import contextlib
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import threading
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, replace
from types import FrameType
from typing import Protocol

PREFIX = "faultlab"
LIMIT = 100_000  # netem's queue, in packets: a long delay at a high rate must not add its own loss
TABLE = "faultlab"
TERMINATING = (signal.SIGTERM, signal.SIGHUP)  # their default action skips atexit


class NetlabError(RuntimeError):
    """A command netlab ran failed, or the host cannot run netlab."""


class FaultLog(Protocol):
    """Where a lab writes its fault events: history.Recorder, or anything with its `fault`."""

    def fault(self, kind: str, **args: object) -> None: ...


class Runner(Protocol):
    """Runs a command with `stdin`, returns its output; raises NetlabError when it fails."""

    def __call__(self, argv: Sequence[str], stdin: str | None = None, /) -> str: ...


def run(argv: Sequence[str], stdin: str | None = None) -> str:
    """Runs a command, feeding it `stdin`; returns its output, raises NetlabError when it fails."""
    done = subprocess.run(argv, input=stdin, capture_output=True, text=True, check=False)
    if done.returncode:
        raise NetlabError(f"{shlex.join(argv)}: exit {done.returncode}: {done.stderr.strip()}")
    return done.stdout


def unavailable() -> str | None:
    """Why this host cannot run netlab, or None when it can."""
    if sys.platform != "linux":
        return f"netlab needs Linux network namespaces, not {sys.platform}"
    if os.geteuid() != 0:
        return "netlab needs root (ip netns, tc and nft)"
    return None


def _number(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".")


@dataclass(frozen=True)
class Shape:
    """One direction of one link: what its netem qdisc adds."""

    ms: float = 0.0
    loss: float = 0.0  # probabilities, 0 to 1
    reorder: float = 0.0
    duplicate: float = 0.0
    mbit: float = 0.0  # 0 is uncapped

    def netem(self) -> list[str]:
        args = ["limit", str(LIMIT)]
        if self.ms:
            args += ["delay", f"{_number(self.ms)}ms"]
        for name, p in (("loss", self.loss), ("reorder", self.reorder), ("duplicate", self.duplicate)):
            if p:
                args += [name, f"{_number(p * 100)}%"]
        if self.mbit:
            args += ["rate", f"{_number(self.mbit)}mbit"]
        return args


_live: set[Netlab] = set()
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


class Netlab:
    """A namespace for every node, numbered from 1, a veth pair for every pair of nodes."""

    def __init__(self, log: FaultLog | None = None, prefix: str = PREFIX, runner: Runner = run) -> None:
        global _instances
        _instances += 1
        self.log = log
        self.prefix = prefix
        self.run = runner
        self.name = f"{prefix}-{os.getpid()}-{_instances}"
        self.nodes: list[int] = []
        self.ports: dict[int, int] = {}
        self.shapes: dict[tuple[int, int], Shape] = {}  # by directed pair (source, target)
        self.cut: set[tuple[int, int]] = set()  # directed pairs whose packets are dropped

    def _fault(self, kind: str, **args: object) -> None:
        if self.log is not None:
            self.log.fault(kind, **args)

    def namespace(self, node: int) -> str:
        return f"{self.name}-n{node}"

    def ip(self, node: int, peer: int) -> str:
        """`node`'s address on its link to `peer`."""
        lo, hi = sorted((node, peer))
        return f"10.{lo}.{hi}.{node}"

    def address(self, source: int, target: int) -> str:
        """The address node `source` dials to reach `target`."""
        return f"{self.ip(target, source)}:{self.ports[target]}"

    def command(self, node: int, argv: Sequence[str]) -> list[str]:
        """`argv` run inside `node`'s namespace."""
        return ["ip", "netns", "exec", self.namespace(node), *argv]

    def socket(self, node: int, kind: socket.SocketKind = socket.SOCK_STREAM) -> socket.socket:
        """An IPv4 socket in `node`'s namespace, made by this thread entering it and coming back."""
        if sys.platform == "linux":
            with open("/proc/thread-self/ns/net") as home, open(f"/run/netns/{self.namespace(node)}") as there:
                os.setns(there.fileno(), os.CLONE_NEWNET)
                try:
                    return socket.socket(socket.AF_INET, kind)
                finally:
                    os.setns(home.fileno(), os.CLONE_NEWNET)
        raise NetlabError(f"netlab needs Linux network namespaces, not {sys.platform}")

    async def start(self, peer_ports: dict[int, int]) -> None:
        """Builds a namespace per node; each node listens on its port in its own namespace."""
        self.open(peer_ports)

    def open(self, peer_ports: dict[int, int]) -> None:
        """start, for callers without an event loop."""
        if self.nodes:
            raise NetlabError(f"{self.name} is already open")
        if not peer_ports or not all(0 < node < 255 for node in peer_ports):
            raise ValueError("nodes are numbered 1 to 254")
        sweep(self.prefix, self.run)
        _trap()
        self.nodes = sorted(peer_ports)
        self.ports = dict(peer_ports)
        _live.add(self)
        try:
            pairs = [(a, b) for a in self.nodes for b in self.nodes if a < b]
            setup = [f"netns add {self.namespace(node)}" for node in self.nodes]
            setup += [
                f"link add to{b} netns {self.namespace(a)} type veth peer name to{a} netns {self.namespace(b)}"
                for a, b in pairs
            ]
            self.run(["ip", "-batch", "-"], "\n".join(setup) + "\n")
            table = (
                f"table inet {TABLE} {{\n"
                "  set cut { type ifname; }\n"
                "  chain input { type filter hook input priority filter - 10; policy accept; iifname @cut drop; }\n"
                "}\n"
            )
            for node in self.nodes:
                peers = [peer for peer in self.nodes if peer != node]
                links = [f"addr add {self.ip(node, peer)}/24 dev to{peer}" for peer in peers]
                links += [f"link set to{peer} up" for peer in peers] + ["link set lo up"]
                self.run(["ip", "-n", self.namespace(node), "-batch", "-"], "\n".join(links) + "\n")
                self.run(self.command(node, ["nft", "-f", "-"]), table)
                self.shapes.update({(node, peer): Shape() for peer in peers})
            self._shape(list(self.shapes))
        except BaseException:
            with contextlib.suppress(NetlabError):
                self.teardown()
            raise

    def teardown(self) -> None:
        """Deletes every namespace this lab made, with its veths, qdiscs and tables; idempotent."""
        nodes = self.nodes
        self.nodes = []
        _live.discard(self)
        self.shapes.clear()
        self.cut.clear()
        if nodes:
            self.run(
                ["ip", "-force", "-batch", "-"], "".join(f"netns delete {self.namespace(node)}\n" for node in nodes)
            )

    async def close(self) -> None:
        self.teardown()

    def links(self, a: int | None = None, b: int | None = None) -> list[tuple[int, int]]:
        """Every directed pair, `a`'s, or the pair (a, b)'s two directions."""
        if a is None:
            return list(self.shapes)
        return [(s, t) for s, t in self.shapes if (a in (s, t) if b is None else {s, t} == {a, b})]

    def _shape(self, links: Iterable[tuple[int, int]]) -> None:
        by_node: dict[int, list[str]] = {}
        for source, target in links:
            netem = " ".join(self.shapes[source, target].netem())
            by_node.setdefault(source, []).append(f"qdisc replace dev to{target} root netem {netem}")
        for node, commands in by_node.items():
            self.run(["tc", "-n", self.namespace(node), "-batch", "-"], "\n".join(commands) + "\n")

    def _reshape(self, links: list[tuple[int, int]], **change: float) -> None:
        shapes = {link: replace(self.shapes[link], **change) for link in links}
        for (source, target), shape in shapes.items():
            if shape.reorder and not shape.ms:
                raise ValueError(f"reorder on {source}->{target} needs a delay: netem reorders by skipping it")
        self.shapes.update(shapes)
        self._shape(links)

    def _cut(self, pairs: Iterable[tuple[int, int]], verb: str) -> None:
        by_target: dict[int, list[str]] = {}
        for source, target in pairs:
            by_target.setdefault(target, []).append(f'"to{source}"')
        for target, names in by_target.items():
            self.run(self.command(target, ["nft", f"{verb} element inet {TABLE} cut {{ {', '.join(names)} }}"]))

    def partition(self, a: int, b: int) -> None:
        """Drops every packet between `a` and `b`, both ways, until heal; TCP retransmits after it."""
        pairs = {(a, b), (b, a)} - self.cut
        self._cut(pairs, "add")
        self.cut |= pairs
        self._fault("partition", nodes=[a, b])

    def isolate(self, node: int) -> None:
        """Partitions `node` from every other node."""
        pairs = set(self.links(node)) - self.cut
        self._cut(pairs, "add")
        self.cut |= pairs
        self._fault("isolate", nodes=[node])

    def heal(self, a: int | None = None, b: int | None = None) -> None:
        """Heals every link, `a`'s links, or the pair (a, b)."""
        pairs = set(self.links(a, b)) & self.cut
        self._cut(pairs, "delete")
        self.cut -= pairs
        self._fault("heal", nodes=[n for n in (a, b) if n is not None])

    def reset(self, a: int, b: int) -> None:
        """Aborts every TCP connection between `a` and `b`: each side's kernel destroys its socket and
        sends the peer a RST (ss -K). New connections work, as after Mesh.reset."""
        alive = ["state", "connected", "exclude", "time-wait", "exclude", "syn-recv"]
        for node, peer in ((a, b), (b, a)):
            self.run(self.command(node, ["ss", "-K", "-t", *alive, "dst", self.ip(peer, node)]))
        for node, peer in ((a, b), (b, a)):
            left = self.run(self.command(node, ["ss", "-H", "-t", *alive, "dst", self.ip(peer, node)]))
            if left.strip():
                raise NetlabError(f"reset left connections (the kernel needs CONFIG_INET_DIAG_DESTROY):\n{left}")
        self._fault("reset", nodes=[a, b])

    @staticmethod
    def _selected(a: int | None, b: int | None) -> dict[str, object]:
        return {"nodes": [n for n in (a, b) if n is not None]} if a is not None else {}

    def cap(self, mbit: float, a: int | None = None, b: int | None = None) -> None:
        """Caps each direction of every link (or `a`'s, or the pair's) at `mbit` megabits per second; 0 lifts it."""
        if mbit < 0:
            raise ValueError(f"cap {mbit} is negative")
        self._reshape(self.links(a, b), mbit=mbit)
        self._fault("cap", mbit=mbit, **self._selected(a, b))

    def delay(self, ms: float, a: int | None = None, b: int | None = None) -> None:
        """Delays each direction by `ms` milliseconds, so a round trip by twice that; 0 lifts it."""
        if ms < 0:
            raise ValueError(f"delay {ms} is negative")
        self._reshape(self.links(a, b), ms=ms)
        self._fault("delay", ms=ms, **self._selected(a, b))

    def _probability(self, kind: str, p: float, a: int | None, b: int | None) -> None:
        if not 0 <= p <= 1:
            raise ValueError(f"{kind} {p} is not a probability")
        self._reshape(self.links(a, b), **{kind: p})
        self._fault(kind, p=p, **self._selected(a, b))

    def loss(self, p: float, a: int | None = None, b: int | None = None) -> None:
        """Drops each packet with probability `p`."""
        self._probability("loss", p, a, b)

    def reorder(self, p: float, a: int | None = None, b: int | None = None) -> None:
        """Sends each packet with probability `p` ahead of the delay, so it overtakes delayed ones."""
        self._probability("reorder", p, a, b)

    def duplicate(self, p: float, a: int | None = None, b: int | None = None) -> None:
        """Sends each packet twice with probability `p`."""
        self._probability("duplicate", p, a, b)


def sweep(prefix: str = PREFIX, runner: Runner = run, everything: bool = False) -> list[str]:
    """Deletes the namespaces named with `prefix` whose owner died (or all of them); returns their names."""
    owned = re.compile(rf"^{re.escape(prefix)}-(\d+)-\d+-n\d+$")
    stale: list[str] = []
    for line in runner(["ip", "netns", "list"]).splitlines():
        name = line.split(" ", 1)[0]
        if (match := owned.match(name)) and (everything or not _alive(int(match[1]))):
            stale.append(name)
    if stale:
        runner(["ip", "-force", "-batch", "-"], "".join(f"netns delete {name}\n" for name in stale))
    return stale


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="netlab.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    clean = sub.add_parser("sweep", help="delete the namespaces of labs whose owner died")
    clean.add_argument("--prefix", default=PREFIX)
    clean.add_argument("--all", action="store_true", help="also those of live owners")
    args = parser.parse_args(argv)
    if (reason := unavailable()) is not None:
        print(f"netlab.py: {reason}", file=sys.stderr)
        return 2
    removed = sweep(args.prefix, everything=args.all)
    print(f"swept {len(removed)} namespace(s){': ' if removed else ''}{' '.join(removed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
