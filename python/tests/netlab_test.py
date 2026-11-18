#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""netlab writes a fault event per call, makes each fault it names, and leaves nothing behind.

Everywhere, a lab over a recording runner checks the commands each call makes and that every call
writes its fault event into a log history.py reads beside operations. On Linux as root, probes
between namespaces measure each fault (delay, loss, reorder, duplicate, cap, partition, isolate,
reset, heal), and teardown is checked after close, exit and SIGTERM, SIGHUP, SIGINT and SIGKILL,
listing namespaces, veths and nftables tables before and after. Elsewhere the live part prints
SKIP with its reason; `--live` makes that a failure.

python3 -I tests/netlab_test.py [--live]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import select
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from faultlab.history import Recorder, load_log
from faultlab.netlab import Netlab, NetlabError, run, sweep, unavailable

SCRATCH = Path(__file__).resolve().parents[2] / ".scratch" / "netlab_test"
PORT = 7000
A3_PREFIX = "faultlab-a3"
SO_RCVBUFFORCE = 33  # Linux; root may pass rmem_max

results: list[tuple[str, bool]] = []


def verdict(label: str, ok: bool) -> bool:
    results.append((label, ok))
    print(f"  {'ok:  ' if ok else 'FAIL:'} {label}", flush=True)
    return ok


class Recording:
    """A runner that records each command instead of running it."""

    def __init__(self) -> None:
        self.calls: list[tuple[list[str], str | None]] = []

    def __call__(self, argv: Sequence[str], stdin: str | None = None, /) -> str:
        self.calls.append((list(argv), stdin))
        return ""

    def take(self) -> list[str]:
        calls = [" ".join(argv) + (f" <<< {stdin.strip()}" if stdin else "") for argv, stdin in self.calls]
        self.calls.clear()
        return calls


def events() -> None:
    """Every call writes its fault event; history.py reads them between operations (A2)."""
    print("events: a recording lab", flush=True)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    path = SCRATCH / "events.jsonl"
    path.unlink(missing_ok=True)
    recorder = Recorder(path, scenario="faultlab/1 name=netlab seed=1")
    runner = Recording()
    lab = Netlab(recorder, prefix="faultlab-unit", runner=runner)
    lab.open({1: 9001, 2: 9002, 3: 9003})
    setup = runner.take()
    ns = lab.namespace
    verdict(
        "open makes a namespace per node and a veth pair per pair of nodes, nothing in the root namespace",
        any(f"netns add {ns(3)}" in call for call in setup)
        and any(f"link add to2 netns {ns(1)} type veth peer name to1 netns {ns(2)}" in call for call in setup)
        and sum("type veth" in call for call in setup) == 1  # one ip batch carries all three pairs
        and sum(call.count("type veth") for call in setup) == 3,
    )
    verdict("address(1, 2) is node 2's end of link 1-2", lab.address(1, 2) == "10.1.2.2:9002")

    expected: list[tuple[str, dict[str, object]]] = []
    commands: dict[str, list[str]] = {}

    def call(kind: str, args: dict[str, object], label: str, action: Callable[[], None]) -> None:
        recorder.record(f"c{len(expected)}", "write", "k", f"v{len(expected)}", "quorum", "invoke")
        action()
        commands[label] = runner.take()
        expected.append((kind, args))
        recorder.record(f"c{len(expected) - 1}", "write", "k", f"v{len(expected) - 1}", "quorum", "ok")

    call("partition", {"nodes": [1, 2]}, "lab.partition(1, 2)", lambda: lab.partition(1, 2))
    call("heal", {"nodes": [1, 2]}, "lab.heal(1, 2)", lambda: lab.heal(1, 2))
    call("isolate", {"nodes": [3]}, "lab.isolate(3)", lambda: lab.isolate(3))
    call("heal", {"nodes": []}, "lab.heal()", lambda: lab.heal())
    call("reset", {"nodes": [1, 2]}, "lab.reset(1, 2)", lambda: lab.reset(1, 2))
    call("cap", {"mbit": 20}, "lab.cap(20)", lambda: lab.cap(20))
    call("delay", {"ms": 50, "nodes": [1, 2]}, "lab.delay(50, 1, 2)", lambda: lab.delay(50, 1, 2))
    call("loss", {"p": 0.1}, "lab.loss(0.1)", lambda: lab.loss(0.1))
    call("reorder", {"p": 0.25, "nodes": [1, 2]}, "lab.reorder(0.25, 1, 2)", lambda: lab.reorder(0.25, 1, 2))
    call("duplicate", {"p": 0.05, "nodes": [3]}, "lab.duplicate(0.05, 3)", lambda: lab.duplicate(0.05, 3))

    nft = commands["lab.partition(1, 2)"]
    verdict(
        "partition(1, 2) adds one nft element per directed pair, in the receiving namespace",
        len(nft) == 2
        and any(c.startswith(f'ip netns exec {ns(2)} nft add element inet faultlab cut {{ "to1" }}') for c in nft)
        and any(c.startswith(f'ip netns exec {ns(1)} nft add element inet faultlab cut {{ "to2" }}') for c in nft),
    )
    verdict(
        "isolate(3) cuts the four directed pairs touching 3, heal() deletes them",
        sum(c.count('"to') for c in commands["lab.isolate(3)"]) == 4
        and sum(c.count('"to') for c in commands["lab.heal()"]) == 4
        and all(" delete element " in c for c in commands["lab.heal()"]),
    )
    verdict(
        "reset(1, 2) destroys the pair's sockets on both sides (ss -K), then checks none is left",
        [c.split()[4:6] for c in commands["lab.reset(1, 2)"]]
        == [["ss", "-K"], ["ss", "-K"], ["ss", "-H"], ["ss", "-H"]],
    )
    shaped = commands["lab.reorder(0.25, 1, 2)"]
    verdict(
        "reorder(0.25, 1, 2) reshapes only the pair's two directions, keeping delay, loss and rate",
        len(shaped) == 2 and all("delay 50ms loss 10% reorder 25% rate 20mbit" in c for c in shaped),
    )
    try:
        lab.reorder(0.5, 2, 3)
        refused = False
    except ValueError:
        refused = True
    verdict("reorder without a delay raises ValueError and writes no event", refused and not runner.take())

    lab.teardown()
    verdict(
        "teardown deletes the namespaces, which hold everything else",
        runner.take() == [f"ip -force -batch - <<< netns delete {ns(1)}\nnetns delete {ns(2)}\nnetns delete {ns(3)}"],
    )
    recorder.close()

    log = load_log(path.read_text().splitlines())
    seen = [(fault.kind, fault.args) for fault in log.faults]
    verdict(
        "each call wrote its fault event, in order, with its arguments",
        seen == expected and log.scenario == "faultlab/1 name=netlab seed=1",
    )
    verdict("history.py reads the operations between them", len(log.ops) == len(expected))
    times = [json.loads(line)["time"] for line in path.read_text().splitlines()[1:]]
    verdict("operations and faults share one clock", times == sorted(times))


# The live probes: Linux, root.


def receive(sock: socket.socket, seconds: float) -> list[int]:
    """The sequence numbers that arrive within `seconds` of the last one."""
    got: list[int] = []
    while select.select([sock], [], [], seconds)[0]:
        got.append(struct.unpack("!I", sock.recv(64)[:4])[0])
    return got


def flood(lab: Netlab, count: int, gap: int = 100) -> list[int]:
    """Sends `count` numbered datagrams from node 1 to node 2; returns what node 2 received, in arrival order."""
    rx = lab.socket(2, socket.SOCK_DGRAM)
    tx = lab.socket(1, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, SO_RCVBUFFORCE, 1 << 23)  # a burst must not overflow it
    rx.bind((lab.ip(2, 1), PORT))
    got: list[int] = []
    try:
        for start in range(0, count, gap):
            for seq in range(start, min(count, start + gap)):
                tx.sendto(struct.pack("!I", seq), (lab.ip(2, 1), PORT))
            got += receive(rx, 0)
        got += receive(rx, 0.5)
    finally:
        rx.close()
        tx.close()
    return got


def rtt(lab: Netlab, rounds: int = 30) -> float:
    """The median UDP round trip from node 1 to node 2 and back, in milliseconds."""
    echo = lab.socket(2, socket.SOCK_DGRAM)
    echo.bind((lab.ip(2, 1), PORT + 1))
    stop = threading.Event()

    def serve() -> None:
        while not stop.is_set():
            if select.select([echo], [], [], 0.1)[0]:
                data, peer = echo.recvfrom(64)
                echo.sendto(data, peer)

    server = threading.Thread(target=serve)
    server.start()
    probe = lab.socket(1, socket.SOCK_DGRAM)
    probe.settimeout(5)
    samples: list[float] = []
    try:
        for i in range(rounds):
            began = time.monotonic()
            probe.sendto(struct.pack("!I", i), (lab.ip(2, 1), PORT + 1))
            while struct.unpack("!I", probe.recv(64))[0] != i:
                pass
            samples.append((time.monotonic() - began) * 1000)
    finally:
        stop.set()
        server.join()
        probe.close()
        echo.close()
    return statistics.median(samples)


def goodput(lab: Netlab, seconds: float = 4.0) -> float:
    """Megabits per second a bulk TCP stream from node 1 delivers to node 2, after its first second."""
    listener = lab.socket(2)
    listener.bind((lab.ip(2, 1), PORT + 2))
    listener.listen()
    counted = [0.0, 0.0, 0.0]  # bytes in the window, its start, its end

    def sink() -> None:
        conn, _ = listener.accept()
        began = time.monotonic()
        with conn:
            while data := conn.recv(1 << 16):
                now = time.monotonic()
                if now - began >= 1.0:
                    if not counted[1]:
                        counted[1] = now
                    else:
                        counted[0] += len(data)
                    counted[2] = now

    reader = threading.Thread(target=sink)
    reader.start()
    sender = lab.socket(1)
    sender.connect((lab.ip(2, 1), PORT + 2))
    block = bytes(1 << 16)
    ends = time.monotonic() + seconds
    while time.monotonic() < ends:
        sender.sendall(block)
    sender.close()
    reader.join()
    listener.close()
    return counted[0] * 8 / (counted[2] - counted[1]) / 1e6


def connects(lab: Netlab, source: int, target: int, timeout: float = 1.0) -> bool:
    listener = lab.socket(target)
    listener.bind((lab.ip(target, source), PORT + 3))
    listener.listen()
    client = lab.socket(source)
    client.settimeout(timeout)
    try:
        client.connect((lab.ip(target, source), PORT + 3))
        return True
    except (TimeoutError, ConnectionError):
        return False
    finally:
        client.close()
        listener.close()


def pair(lab: Netlab, source: int, target: int, port: int) -> tuple[socket.socket, socket.socket, socket.socket]:
    """An established TCP connection from `source` to `target`: (listener, client, server)."""
    listener = lab.socket(target)
    listener.bind((lab.ip(target, source), port))
    listener.listen()
    client = lab.socket(source)
    client.connect((lab.ip(target, source), port))
    server, _ = listener.accept()
    for sock in (client, server):
        sock.settimeout(5)
    return listener, client, server


def outcome(sock: socket.socket, seconds: float) -> str:
    sock.settimeout(seconds)
    try:
        data = sock.recv(64)
    except TimeoutError:
        return "nothing"
    except ConnectionError as error:
        return type(error).__name__
    return f"bytes {data!r}" if data else "EOF"


def probes() -> None:
    """Each fault, measured between namespaces (A1)."""
    print("probes: three namespaces", flush=True)
    lab = Netlab()
    lab.open({1: PORT, 2: PORT, 3: PORT})
    try:
        base = rtt(lab)
        lab.delay(50, 1, 2)
        added = rtt(lab) - base
        lab.delay(0, 1, 2)
        verdict(
            f"delay 50 ms each way adds {added:.1f} ms to a 1-2 round trip (100 ±10%; base {base:.2f})",
            abs(added - 100) <= 10,
        )

        got = flood(lab, 10_000)
        verdict(f"no loss: {len(got)} of 10000 datagrams arrive, none twice", sorted(got) == list(range(10_000)))
        lab.loss(0.10, 1, 2)
        lost = 100 - len(set(flood(lab, 10_000))) / 100
        lab.loss(0, 1, 2)
        verdict(f"loss 10%: {lost:.2f}% of 10000 datagrams lost (10 ±2 points)", abs(lost - 10) <= 2)

        lab.duplicate(0.10, 1, 2)
        got = flood(lab, 10_000)
        lab.duplicate(0, 1, 2)
        twice = (len(got) - len(set(got))) / 100
        verdict(f"duplicate 10%: {twice:.2f}% of 10000 datagrams arrive twice (10 ±2 points)", abs(twice - 10) <= 2)

        lab.delay(50, 1, 2)
        plain = flood(lab, 2000, gap=2000)
        lab.reorder(0.25, 1, 2)
        mixed = flood(lab, 2000, gap=2000)
        lab.reorder(0, 1, 2)
        lab.delay(0, 1, 2)

        def overtakes(got: list[int]) -> int:
            where = {seq: i for i, seq in enumerate(got)}
            return sum(1 for seq in range(1, 2000) if seq in where and seq - 1 in where and where[seq] < where[seq - 1])

        share = overtakes(mixed) / 19.99
        verdict(
            f"delay alone keeps order: {overtakes(plain)} of 2000 overtake their predecessor", overtakes(plain) == 0
        )
        verdict(
            f"reorder 25%: {share:.2f}% overtake their predecessor (p(1-p) = 18.75 ±3 points)",
            abs(share - 18.75) <= 3,
        )

        lab.cap(20, 1, 2)
        mbit = goodput(lab)
        lab.cap(0, 1, 2)
        verdict(f"cap 20 Mb/s: TCP delivers {mbit:.2f} Mb/s (20 ±10%)", abs(mbit / 20 - 1) <= 0.10)

        listener, client, server = pair(lab, 1, 2, PORT + 10)
        lab.partition(1, 2)
        client.sendall(b"held")
        during = outcome(server, 1.0)
        refused = not connects(lab, 1, 2)
        unaffected = connects(lab, 1, 3)
        lab.heal(1, 2)
        after = outcome(server, 10.0)
        verdict(
            f"partition(1, 2): the far side sees {during}, a new connection "
            f"{'times out' if refused else 'connects'}, 1-3 {'connects' if unaffected else 'fails'}; "
            f"heal delivers {after}",
            during == "nothing" and refused and unaffected and after == "bytes b'held'",
        )
        verdict("heal(1, 2): a new connection connects", connects(lab, 1, 2))
        for sock in (listener, client, server):
            sock.close()

        lab.isolate(1)
        cut = [connects(lab, 1, 2), connects(lab, 3, 1)]
        kept = connects(lab, 2, 3)
        lab.heal(1)
        verdict(
            "isolate(1): 1-2 and 3-1 time out while 2-3 connects; heal(1) restores 1-2",
            cut == [False, False] and kept and connects(lab, 1, 2),
        )

        listener, client, server = pair(lab, 1, 2, PORT + 11)
        other = pair(lab, 1, 3, PORT + 12)
        lab.reset(1, 2)
        sides = [outcome(client, 2.0), outcome(server, 2.0)]
        other[1].sendall(b"alive")
        survived = outcome(other[2], 2.0)
        verdict(
            f"reset(1, 2): the client sees {sides[0]}, the server {sides[1]}; 1-3 still carries {survived}",
            all(side.startswith("Connection") for side in sides) and survived == "bytes b'alive'",
        )
        verdict("reset(1, 2): a new connection connects", connects(lab, 1, 2))
        for sock in (listener, client, server, *other):
            sock.close()
    finally:
        lab.teardown()


# Teardown (A3): a child builds a lab and ends one way; nothing of it may stay.


def listing() -> str:
    """Every namespace, every veth in the root namespace, and every nftables table there."""
    return (
        "namespaces: " + " ".join(sorted(line.split()[0] for line in run(["ip", "netns", "list"]).splitlines())) + "\n"
        "root veths: "
        + " ".join(
            sorted(
                line.split(":")[1].strip() for line in run(["ip", "-o", "link", "show", "type", "veth"]).splitlines()
            )
        )
        + "\n"
        "root nft tables: " + " ".join(sorted(run(["nft", "list", "tables"]).split("\n"))).strip()
    )


def ours() -> list[str]:
    return [line.split()[0] for line in run(["ip", "netns", "list"]).splitlines() if line.startswith(A3_PREFIX + "-")]


def child(end: str) -> int:
    lab = Netlab(prefix=A3_PREFIX)
    lab.open({1: PORT, 2: PORT, 3: PORT})
    lab.partition(1, 2)
    lab.isolate(3)
    lab.delay(5)
    lab.loss(0.01, 1, 3)
    lab.cap(50, 2, 3)
    links = run(lab.command(1, ["ip", "-o", "link", "show", "type", "veth"])).count("to")
    tables = run(lab.command(2, ["nft", "list", "ruleset"])).count("@cut")
    print(f"ready {links} veths in node 1, {tables} cut rule in node 2", flush=True)
    try:
        sys.stdin.readline()  # the parent has counted the namespaces; a signal may come instead
    except KeyboardInterrupt:
        return 130
    if end == "close":
        asyncio.run(lab.close())
    return 0


def teardown() -> None:
    print("teardown: a child lab, ended each way", flush=True)
    before = listing()
    print("  before:\n    " + before.replace("\n", "\n    "), flush=True)
    for end in ("close", "exit", "SIGTERM", "SIGHUP", "SIGINT", "SIGKILL"):
        proc = subprocess.Popen(
            [sys.executable, "-I", __file__, "child", end], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
        )
        assert proc.stdin is not None and proc.stdout is not None
        ready = proc.stdout.readline().strip()
        built = len(ours())
        if end.startswith("SIG"):
            proc.send_signal(getattr(signal, end))
        else:
            proc.stdin.write("go\n")
            proc.stdin.flush()
        proc.wait(30)
        left = ours()
        if end == "SIGKILL":
            swept = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    str(Path(__file__).resolve().parents[1] / "faultlab" / "netlab.py"),
                    "sweep",
                    "--prefix",
                    A3_PREFIX,
                ],
                capture_output=True,
                text=True,
                check=False,
            ).stdout.strip()
            verdict(
                f"SIGKILL: {ready}; leaves {len(left)} of {built} namespaces, which sweep removes ({swept}); "
                f"none left: {not ours()}",
                built == 3 and len(left) == 3 and not ours(),
            )
        else:
            verdict(f"{end}: {ready}; {built} namespaces built, {len(left)} left", built == 3 and not left)
    after = listing()
    print("  after:\n    " + after.replace("\n", "\n    "), flush=True)
    verdict("the listing after equals the listing before", after == before)
    proc = subprocess.Popen(
        [sys.executable, "-I", __file__, "child", "SIGKILL"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
    )
    assert proc.stdout is not None
    proc.stdout.readline()
    proc.kill()
    proc.wait(30)
    stale = len(ours())
    lab = Netlab(prefix=A3_PREFIX)
    lab.open({1: PORT, 2: PORT})
    mine = ours()
    lab.teardown()
    verdict(
        f"a new lab sweeps the {stale} namespaces of a killed owner, keeping only its own {len(mine)}",
        stale == 3 and len(mine) == 2 and all(name.startswith(lab.name) for name in mine) and not ours(),
    )


def main(argv: list[str]) -> int:
    if argv[:1] == ["child"]:
        return child(argv[1])
    parser = argparse.ArgumentParser(prog="netlab_test.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("--live", action="store_true", help="fail, rather than skip, where netlab cannot run")
    args = parser.parse_args(argv)
    events()
    if (reason := unavailable()) is None:
        try:
            probes()
            teardown()
        except NetlabError as error:
            verdict(f"netlab: {error}", False)
        finally:
            sweep(A3_PREFIX, everything=True)
    elif args.live:
        verdict(f"live probes: {reason}", False)
    else:
        print(f"SKIP live probes and teardown: {reason}", flush=True)
    failures = [label for label, ok in results if not ok]
    print(f"=== VERDICT: {'GREEN' if not failures else f'RED ({len(failures)} failures)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
