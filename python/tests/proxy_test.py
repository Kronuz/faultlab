#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""A partitioned link delivers nothing, and healing delivers what it held, in order.

For each event (bytes, a FIN, a reset, from either side, and a new connection), a connection
through one link sees the event happen on its side during a partition; the other side must see
nothing for a second, then the event after `heal()`. Then the mesh: partition, heal, isolate and
reset touch exactly their links and write their fault events, the netlab-only faults raise
Unsupported, and close raises a forwarding task's failure.

python3 -I tests/proxy_test.py
"""

from __future__ import annotations

import asyncio
import socket
import struct
import sys
from pathlib import Path
from typing import override

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from faultlab.proxy import Link, Mesh, Unsupported

QUIET = 1.0

type Stream = tuple[asyncio.StreamReader, asyncio.StreamWriter]


async def seen(reader: asyncio.StreamReader, seconds: float) -> str:
    try:
        data = await asyncio.wait_for(reader.read(65536), seconds)
    except TimeoutError:
        return "nothing"
    except (ConnectionError, OSError):
        return "reset"
    return f"bytes {data!r}" if data else "EOF"


def rst(writer: asyncio.StreamWriter) -> None:
    sock = writer.get_extra_info("socket")
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    writer.transport.abort()


async def case(event: str, side: str) -> bool:
    accepted: asyncio.Queue[Stream] = asyncio.Queue()

    async def target(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await accepted.put((reader, writer))
        await asyncio.sleep(3600)

    server = await asyncio.start_server(target, "127.0.0.1", 0)
    link = Link(1, 2, int(server.sockets[0].getsockname()[1]))
    await link.start()
    try:
        client = await asyncio.open_connection("127.0.0.1", link.port)
        client[1].write(b"hi")
        far = await asyncio.wait_for(accepted.get(), 5)
        assert await far[0].readexactly(2) == b"hi"
        link.partition()
        if event == "connect":
            await asyncio.open_connection("127.0.0.1", link.port)
            during = "a connection" if not accepted.empty() else "nothing"
            await asyncio.sleep(QUIET)
            during = "a connection" if not accepted.empty() else during
            link.heal()
            after = "a connection" if await asyncio.wait_for(accepted.get(), 5) else "nothing"
            expected = "a connection"
        else:
            actor, observer = (client, far) if side == "client" else (far, client)
            if event == "bytes":
                actor[1].write(b"held")
                await actor[1].drain()
                expected = "bytes b'held'"
            elif event == "FIN":
                actor[1].write_eof()
                expected = "EOF"
            else:
                rst(actor[1])
                expected = "reset or EOF"
            during = await seen(observer[0], QUIET)
            link.heal()
            after = await seen(observer[0], 5)
            if event == "reset" and after in ("reset", "EOF"):
                after = expected
        ok = during == "nothing" and after == expected
        label = event if event == "connect" else f"{side} {event}"
        print(f"  {'ok:  ' if ok else 'FAIL:'} {label}: partitioned, the other side sees {during}; healed, {after}")
        return ok
    finally:
        await link.close()
        server.close()


class Events:
    def __init__(self) -> None:
        self.seen: list[tuple[str, dict[str, object]]] = []

    def fault(self, kind: str, **args: object) -> None:
        self.seen.append((kind, args))


async def mesh() -> list[bool]:
    async def idle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await reader.read()
        writer.close()

    servers = {i: await asyncio.start_server(idle, "127.0.0.1", 0) for i in (1, 2, 3)}
    events = Events()
    net = Mesh(events)
    await net.start({i: int(server.sockets[0].getsockname()[1]) for i, server in servers.items()})

    def closed() -> set[tuple[int, int]]:
        return {pair for pair, link in net.links.items() if not link.open.is_set()}

    results: list[tuple[str, bool]] = []
    net.partition(1, 2)
    results.append(("partition(1, 2) holds both directions of that pair only", closed() == {(1, 2), (2, 1)}))
    net.heal(1, 2)
    net.isolate(3)
    results.append(("isolate(3) holds every link of node 3", closed() == {(1, 3), (3, 1), (2, 3), (3, 2)}))
    net.heal(3)
    results.append(("heal(3) heals them", not closed()))
    net.partition(1, 3)
    net.partition(2, 3)
    net.heal()
    results.append(("heal() heals every link", not closed()))
    net.reset(1, 2)
    net.cap(10)
    unsupported = 0
    for fault in (net.delay, net.loss, net.reorder, net.duplicate):
        try:
            fault(1)
        except Unsupported:
            unsupported += 1
    results.append(("delay, loss, reorder and duplicate raise Unsupported", unsupported == 4))
    kinds = [kind for kind, _ in events.seen]
    results.append(
        (
            "each fault writes its event",
            kinds == ["partition", "heal", "isolate", "heal", "partition", "partition", "heal", "reset", "cap"]
            and events.seen[0][1] == {"nodes": [1, 2]}
            and events.seen[-1][1] == {"mbit": 10},
        )
    )
    await net.close()

    class Failing(Link):
        @override
        async def forward(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            writer.close()
            raise RuntimeError("forward failed")

    broken = Mesh()
    link = Failing(1, 2, int(servers[2].sockets[0].getsockname()[1]))
    broken.links[1, 2] = link
    await link.start()
    _, writer = await asyncio.open_connection("127.0.0.1", link.port)
    for _ in range(500):
        if link.failures:
            break
        await asyncio.sleep(0.01)
    raised = False
    try:
        await broken.close()
    except RuntimeError:
        raised = True
    writer.close()
    results.append(("close raises a forwarding task's failure", raised))
    for server in servers.values():
        server.close()
    for label, ok in results:
        print(f"  {'ok:  ' if ok else 'FAIL:'} {label}")
    return [ok for _, ok in results]


async def main() -> int:
    results = [await case(event, side) for event in ("bytes", "FIN", "reset") for side in ("client", "target")]
    results.append(await case("connect", "client"))
    results += await mesh()
    failures = results.count(False)
    print(f"=== VERDICT: {'GREEN' if not failures else f'RED ({failures} failures)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
