#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""Register histories and their checker.

A history is the event log (faultlab F12): one JSON line per invocation and one per completion,

    {"client": "get@node2", "op": "get", "key": "probe/1", "value": "t3", "level": "prefix",
     "outcome": "ok", "time": 1.25}

beside a first `scenario` line, `fault` lines and harness records with a `kind` key, which the
checker sets aside (a violation lists the faults inside its operations' interval).

The checker treats each key as a register whose writes carry unique values, and runs five checks:

- written: every read returns the initial null or a value some write on its key had begun to write.
- prefix: one order of the key's writes explains every read as a prefix of it (durability-modes
  D10), monotonic on each node's launch and never behind a `quorum` write that node acknowledged.
- linearizable: the writes and the reads at level `linearizable` linearize (Gibbons and Korach's zones).
- monotonic: a node whose reads are marked `durable` never reads behind what it served, across its
  launches (prefix allows a restarted node to start over).
- seal: a read at level `seal` returns a number no lower than any a completed `seal` read or an
  acknowledged write on its key returned before it began (a monotone counter's freshness).

python3 -I faultlab/history.py FILE [--level prefix|linearizable]
"""

from __future__ import annotations

import argparse
import itertools
import json
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, TextIO

OPS = ("write", "read", "get")
READS = ("read", "get")
OUTCOMES = ("invoke", "ok", "fail", "info")
READ_LEVELS = ("prefix", "linearizable", "seal")
WRITE_LEVELS = ("quorum", "none")
CHECKS = ("written", "prefix", "linearizable", "monotonic", "seal")
INF = float("inf")

type Value = str | int | float | bool | None


class HistoryError(ValueError):
    """A history the checker cannot read."""


@dataclass(frozen=True)
class Op:
    """One invocation and its completion. An unknown completion (`info`) completes at infinity."""

    client: str
    op: str
    key: str
    value: Value
    level: str
    outcome: str
    invoke: float
    complete: float
    line: int
    node: str | None = None  # the node that served or acknowledged it, when the history names one
    launch: int = 1  # that node's launch: a restart starts the next
    durable: bool = False  # the node keeps what it served across launches

    @property
    def is_read(self) -> bool:
        return self.op in READS

    def describe(self) -> str:
        end = "?" if self.complete == INF else f"{self.complete:.3f}"
        where = f" on {self.node}#{self.launch}" if self.node else ""
        return f"{self.op}({self.value!r}) by {self.client}{where} [{self.invoke:.3f}, {end}] line {self.line}"


@dataclass(frozen=True)
class Fault:
    """A fault line: its kind, time, and arguments by name."""

    kind: str
    time: float
    line: int
    args: dict[str, Any] = field(default_factory=dict)  # decoded JSON

    def describe(self) -> str:
        args = "".join(f" {name}={value}" for name, value in self.args.items())
        return f"{self.kind}{args} at {self.time:.3f} (line {self.line})"


@dataclass(frozen=True)
class Log:
    """A whole event log: its operations, faults, and scenario line."""

    ops: list[Op]
    faults: list[Fault]
    scenario: str | None


class Recorder:
    """Appends a history's lines as a harness issues and completes its operations."""

    def __init__(self, path: Path, scenario: str | None = None) -> None:
        self.path = path
        self.file: TextIO = path.open("a", buffering=1)
        self.start = time.monotonic()
        self.last = -1.0
        if scenario is not None:
            self.file.write(json.dumps({"scenario": scenario}) + "\n")

    def _now(self) -> float:
        self.last = max(round(time.monotonic() - self.start, 6), round(self.last + 1e-6, 6))  # strictly increasing
        return self.last

    def record(
        self, client: str, op: str, key: str, value: Value, level: str, outcome: str, **extra: str | int | bool
    ) -> None:
        line = {"client": client, "op": op, "key": key, "value": value, "level": level, "outcome": outcome}
        line["time"] = self._now()
        self.file.write(json.dumps(line | extra) + "\n")

    def fault(self, kind: str, **args: object) -> None:
        self.file.write(json.dumps({"fault": kind, "time": self._now()} | args) + "\n")

    def close(self) -> None:
        self.file.close()


def load(lines: list[str]) -> list[Op]:
    """The operations of a log; its scenario, fault and harness lines are set aside."""
    return load_log(lines).ops


def load_log(lines: list[str]) -> Log:
    """Pairs each client's invocations with their completions; an invocation left open is `info`."""
    ops: list[Op] = []
    faults: list[Fault] = []
    scenario: str | None = None
    pending: dict[str, tuple[int, dict[str, Any]]] = {}  # decoded JSON lines
    finished: dict[str, float] = {}  # each client's last completion
    first = True
    for number, text in enumerate(lines, 1):
        if not text.strip():
            continue
        try:
            entry = json.loads(text)
        except json.JSONDecodeError as error:
            raise HistoryError(f"line {number}: {error}") from None
        if not isinstance(entry, dict):
            raise HistoryError(f"line {number}: not an object")
        was_first, first = first, False
        if "kind" in entry:
            continue  # a harness record
        if "scenario" in entry:
            if not was_first:
                raise HistoryError(f"line {number}: a scenario line after the first")
            scenario = str(entry["scenario"])
            continue
        if "fault" in entry:
            if "time" not in entry:
                raise HistoryError(f"line {number}: a fault without a time")
            args = {name: value for name, value in entry.items() if name not in ("fault", "time")}
            faults.append(Fault(str(entry["fault"]), float(entry["time"]), number, args))
            continue
        for name in ("client", "op", "key", "level", "outcome", "time", "value"):
            if name not in entry:
                raise HistoryError(f"line {number}: no {name!r}")
        client, op, outcome = str(entry["client"]), entry["op"], entry["outcome"]
        if op not in OPS or outcome not in OUTCOMES:
            raise HistoryError(f"line {number}: op {op!r} or outcome {outcome!r} unknown")
        if entry["level"] not in (READ_LEVELS if op in READS else WRITE_LEVELS):
            raise HistoryError(f"line {number}: level {entry['level']!r} is not a {op} level")
        if outcome == "invoke":
            if client in pending:
                raise HistoryError(f"line {number}: {client} invokes while line {pending[client][0]} is open")
            if float(entry["time"]) <= finished.get(client, -INF):  # a tie keeps the client's program order
                entry = entry | {"time": finished[client] + 1e-9}
            pending[client] = (number, entry)
            continue
        if client not in pending:
            raise HistoryError(f"line {number}: {client} completes nothing")
        start, call = pending.pop(client)
        if call["op"] != op or call["key"] != entry["key"]:
            raise HistoryError(f"line {number}: completes a different operation than line {start}")
        value = entry["value"] if op in READS else call["value"]
        complete = INF if outcome == "info" else float(entry["time"])
        finished[client] = float(entry["time"])
        ops.append(_op(call, value, outcome, complete, start))
    for start, call in pending.values():
        ops.append(_op(call, call["value"] if call["op"] == "write" else None, "info", INF, start))
    return Log(ops, faults, scenario)


def _op(call: dict[str, Any], value: object, outcome: str, complete: float, line: int) -> Op:
    if not (value is None or isinstance(value, str | int | float | bool)):
        raise HistoryError(f"line {line}: value {value!r} is not a scalar")
    invoke = float(call["time"])
    return Op(
        str(call["client"]),
        str(call["op"]),
        str(call["key"]),
        value,
        str(call["level"]),
        outcome,
        invoke,
        complete,
        line,
        str(call["node"]) if call.get("node") is not None else None,
        int(call.get("launch", 1)),
        bool(call.get("durable", False)),
    )


type Finding = tuple[str, list[Op]]  # a violation and the operations it names
type Edges = dict[Value, dict[Value, tuple[str, list[Op]]]]  # a: {b: (why a precedes b, the ops behind it)}


def check(
    ops: list[Op], level: str | None = None, disabled: frozenset[str] = frozenset(), faults: list[Fault] | None = None
) -> list[str]:
    """Returns the violations, each with the faults inside its operations' interval. `level`
    overrides every register read's own level; `disabled` exists for the self-test's mutants."""
    by_key: dict[str, list[Op]] = defaultdict(list)
    for op in ops:
        by_key[op.key].append(op)
    findings: list[Finding] = []
    for key, key_ops in sorted(by_key.items()):
        writes: dict[Value, Op] = {}
        for op in key_ops:
            if op.op == "write":
                if op.value is None or op.value in writes:
                    raise HistoryError(f"line {op.line}: key {key!r} writes {op.value!r} twice or writes null")
                writes[op.value] = op
        reads = [op for op in key_ops if op.is_read and op.outcome == "ok"]
        registers = [op for op in reads if op.level != "seal"]  # seal reads have their own check
        if "written" not in disabled:
            findings += _written(key, reads, writes)
        if "prefix" not in disabled:
            findings += _prefix(key, registers, writes)
        if "linearizable" not in disabled:
            chosen = [op for op in registers if (level or op.level) == "linearizable"]
            findings += _linearizable(key, chosen, writes)
        if "monotonic" not in disabled:
            findings += _monotonic(key, registers, writes)
        if "seal" not in disabled:
            findings += _seal(key, reads, writes)
    return [_report(text, involved, faults or []) for text, involved in findings]


def _report(text: str, involved: list[Op], faults: list[Fault]) -> str:
    if not involved or not faults:
        return text
    begin = min(op.invoke for op in involved)
    end = max(op.complete for op in involved)
    inside = [fault.describe() for fault in faults if begin <= fault.time <= end]
    return text + (f"; faults inside: {', '.join(inside)}" if inside else "")


def _source(read: Op, writes: dict[Value, Op]) -> Op | None:
    """The write a read observed, if it could have observed it."""
    write = writes.get(read.value)
    if write is None or write.outcome == "fail" or write.invoke > read.complete:
        return None
    return write


def _written(key: str, reads: list[Op], writes: dict[Value, Op]) -> list[Finding]:
    return [
        (f"{key}: written: {read.describe()} returns a value no write had begun to write", [read])
        for read in reads
        if read.value is not None and _source(read, writes) is None
    ]


def _write_order(writes: dict[Value, Op]) -> Edges:
    """A settled write that completed before another began precedes it; a `none` write may commit later."""
    edges: Edges = defaultdict(dict)
    for a, first in writes.items():
        for b, second in writes.items():
            settled = first.outcome == "ok" and first.level == "quorum"
            if settled and second.outcome != "fail" and first.complete < second.invoke:
                why = f"{first.describe()} completed before {second.describe()} began"
                edges[a].setdefault(b, (why, [first, second]))
    return edges


def _order(key: str, name: str, edges: Edges) -> list[Finding]:
    """A cycle among the "before" edges: no one order of the writes explains the reads."""
    cycle = _cycle(edges)
    if not cycle:
        return []
    steps = [edges[a][b] for a, b in itertools.pairwise(cycle)]
    text = f"{key}: {name}: no one order of the writes explains the reads: " + "; ".join(why for why, _ in steps)
    return [(text, [op for _, ops in steps for op in ops])]


def _prefix(key: str, reads: list[Op], writes: dict[Value, Op]) -> list[Finding]:
    """Consistent prefix (durability-modes D10): some one order of the writes explains every read.

    A read of v sees the prefix that ends at v (null: the empty prefix). The order must put a write
    that completed before another began first; each node's launch reads monotonically in real
    time; and a node's read is never behind a `quorum` write that node acknowledged before it.
    Each rule is a "before" edge between writes, so one order exists when the edges have no cycle.
    A `none` write may commit after its acknowledgement, so it orders nothing by real time. A read
    whose history line names no node may come from any replica, so only the order binds it.
    """
    findings: list[Finding] = []
    edges = _write_order(writes)

    def before(a: Value, b: Value, why: str, involved: list[Op]) -> None:
        if a != b and a is not None:
            if b is None:
                findings.append((f"{key}: prefix: {why}", involved))
            else:
                edges[a].setdefault(b, (why, involved))

    seen = [read for read in reads if read.value is None or _source(read, writes) is not None]
    by_replica: dict[tuple[str, int], list[Op]] = defaultdict(list)
    for read in seen:
        if read.node is not None:
            by_replica[read.node, read.launch].append(read)
    for replica in by_replica.values():
        for first in replica:
            for second in replica:
                if first.complete < second.invoke:
                    why = f"{second.describe()} reads behind {first.describe()}"
                    before(first.value, second.value, why, [first, second])
    for read in seen:
        for write in writes.values():
            durable = write.outcome == "ok" and write.level == "quorum" and write.complete < read.invoke
            if durable and read.node is not None and write.node == read.node:
                why = f"{read.describe()} reads behind {write.describe()}, acknowledged there"
                before(write.value, read.value, why, [write, read])
    return findings + _order(key, "prefix", edges)


def _monotonic(key: str, reads: list[Op], writes: dict[Value, Op]) -> list[Finding]:
    """A node marked durable reads in one order with the writes, across its launches.

    Its `durable` reads, in real time on the node, are "before" edges between the values they
    read, whatever the launch, beside the writes' real-time order; a cycle, or a null read after
    a value, means the node served older history than it had served.
    """
    by_node: dict[str, list[Op]] = defaultdict(list)
    for read in reads:
        if read.durable and read.node is not None and (read.value is None or _source(read, writes) is not None):
            by_node[read.node].append(read)
    if not by_node:
        return []
    findings: list[Finding] = []
    edges = _write_order(writes)
    for node_reads in by_node.values():
        for first in node_reads:
            for second in node_reads:
                if first.complete < second.invoke and first.value is not None and first.value != second.value:
                    why = f"{second.describe()} reads behind {first.describe()}"
                    if second.value is None:
                        findings.append((f"{key}: monotonic: {why}", [first, second]))
                    else:
                        edges[first.value].setdefault(second.value, (why, [first, second]))
    return findings + _order(key, "monotonic", edges)


def _number(op: Op, key: str) -> float:
    if isinstance(op.value, bool) or not isinstance(op.value, int | float):
        raise HistoryError(f"line {op.line}: key {key!r} holds seals, and {op.value!r} is not a number")
    return float(op.value)


def _seal(key: str, reads: list[Op], writes: dict[Value, Op]) -> list[Finding]:
    """A `seal` read returns no less than any completed seal read or acknowledged write had
    returned before it began: the read half of linearizability over a monotone counter."""
    seals = [op for op in reads if op.level == "seal"]
    if not seals:
        return []
    observed = [op for op in seals + [w for w in writes.values() if w.outcome == "ok"] if op.value is not None]
    values = {id(op): _number(op, key) for op in observed}
    findings: list[Finding] = []
    for read in seals:
        floor = [op for op in observed if op.complete < read.invoke]
        if not floor:
            continue
        highest = max(floor, key=lambda op: values[id(op)])
        if read.value is None or _number(read, key) < values[id(highest)]:
            findings.append((f"{key}: seal: {read.describe()} returns less than {highest.describe()}", [highest, read]))
    return findings


def _cycle(edges: Edges) -> list[Value]:
    """A cycle of the graph as a closed walk [a, ..., a], or [] when it has none."""
    state: dict[Value, int] = {}  # 1 on the stack, 2 done
    for root in list(edges):
        if root in state:
            continue
        stack: list[tuple[Value, list[Value]]] = [(root, list(edges[root]))]
        path = [root]
        state[root] = 1
        while stack:
            node, todo = stack[-1]
            if not todo:
                state[node] = 2
                stack.pop()
                path.pop()
                continue
            nxt = todo.pop()
            if state.get(nxt) == 1:
                return [*path[path.index(nxt) :], nxt]
            if nxt not in state:
                state[nxt] = 1
                path.append(nxt)
                stack.append((nxt, list(edges.get(nxt, {}))))
    return []


def _linearizable(key: str, reads: list[Op], writes: dict[Value, Op]) -> list[Finding]:
    """Gibbons and Korach's test for a register with unique writes whose reads name their write.

    A cluster is a write and the reads of its value; its zone runs from the cluster's earliest
    completion to its latest invocation, forward when the first is earlier. The history is
    linearizable when no read precedes its write, no two forward zones overlap, and no backward
    zone lies inside a forward one. A write nobody read may be dropped (it could take effect last),
    so only acknowledged `quorum` writes and the writes some read observed form clusters. A `none`
    write is acknowledged before it commits (cluster-release D4), so an observed one completes at
    infinity, as an `info` write does.
    """
    clusters: dict[Value, list[Op]] = defaultdict(list)
    findings: list[Finding] = []
    for read in reads:
        write = _source(read, writes) if read.value is not None else None
        if read.value is not None and write is None:
            continue  # the written check names it
        clusters[read.value].append(read)
    initial = Op("initial", "write", key, None, "quorum", "ok", -INF, -INF, 0)
    members: dict[Value, list[Op]] = {None: [initial]}
    for value, write in writes.items():
        required = write.outcome == "ok" and write.level == "quorum"
        if required or value in clusters:
            members[value] = [replace(write, complete=INF) if write.level == "none" else write]
    forward: list[tuple[float, float, Value]] = []
    backward: list[tuple[float, float, Value]] = []
    for value, group in members.items():
        group += clusters.get(value, [])
        first_end = min(op.complete for op in group)
        last_start = max(op.invoke for op in group)
        if first_end < last_start:
            forward.append((first_end, last_start, value))
        else:
            backward.append((last_start, first_end, value))
    forward.sort(key=lambda zone: zone[0])

    def involved(*values: Value) -> list[Op]:
        return [op for value in values for op in members[value] if op.line]

    for (_, end, a), (start, _, b) in itertools.pairwise(forward):
        if start < end:
            text = f"{key}: linearizable: {_cluster(a, members)} and {_cluster(b, members)} overlap"
            findings.append((text, involved(a, b)))
    for start, end, value in backward:
        for outer_start, outer_end, outer in forward:
            if outer_start < start and end < outer_end:
                text = f"{key}: linearizable: {_cluster(value, members)} falls inside {_cluster(outer, members)}"
                findings.append((text, involved(value, outer)))
    return findings


def _cluster(value: Value, members: dict[Value, list[Op]]) -> str:
    group = members[value]
    reads = [op for op in group if op.is_read]
    head = "the initial null" if value is None else group[0].describe()
    tail = f" and its read {reads[-1].describe()}" if reads else ""
    return head + tail


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="history.py", description="Check a register history.")
    parser.add_argument("history", type=Path)
    parser.add_argument(
        "--level", choices=("prefix", "linearizable"), help="check every register read at this level, not its own"
    )
    args = parser.parse_args(argv)
    try:
        log = load_log(args.history.read_text().splitlines())
        violations = check(log.ops, args.level, faults=log.faults)
    except (HistoryError, OSError) as error:
        print(f"history: {error}", file=sys.stderr)
        return 2
    keys = len({op.key for op in log.ops})
    for violation in violations:
        print(f"  violation: {violation}")
    verdict = "VIOLATIONS" if violations else "ok"
    faults = f", {len(log.faults)} faults" if log.faults else ""
    print(f"history: {len(log.ops)} operations on {keys} keys{faults} at {args.level or 'their own levels'}: {verdict}")
    if violations and log.scenario:
        print(f"replay: FAULTLAB_SCENARIO='{log.scenario}'")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
