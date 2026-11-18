#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""The seeded scenario line (faultlab F11), as include/faultlab/scenario.h reads and writes it.

    faultlab/1 name=mirror_crash.first seed=11 cut=513 pick=all,additions,current,old

Space-separated key=value after the version; `name` and `seed` required; `cut`, `pick` (epochs,
names, size, sectors) and further keys optional. FAULTLAB_SCENARIO=<line> runs exactly that case.
All of a case's randomness comes from `random()`, seeded by `seed`.
"""

from __future__ import annotations

import os
import random
from dataclasses import dataclass, field

VERSION = "faultlab/1"
PICKS = (
    ("coin", "none", "all"),
    ("coin", "durable", "all", "additions"),
    ("coin", "durable", "current", "sector"),
    ("coin", "old", "fresh"),
)
RESERVED = ("name", "seed", "cut", "pick")


class ScenarioError(ValueError):
    """A scenario line that does not parse."""


def _word(what: str, word: str) -> str:
    if not word or any(c in word for c in " \t\r\n="):
        raise ScenarioError(f"{what} must be one word without '='")
    return word


def _number(key: str, value: str) -> int:
    if not value.isdigit() or not value.isascii() or int(value) >= 1 << 64:
        raise ScenarioError(f"{key} is not a 64-bit number: {value}")
    return int(value)


@dataclass
class Scenario:
    name: str
    seed: int
    cut: int | None = None
    pick: tuple[str, str, str, str] | None = None
    extra: dict[str, str] = field(default_factory=dict)  # further keys, in order

    def random(self) -> random.Random:
        return random.Random(self.seed)

    def line(self) -> str:
        words = [VERSION, f"name={_word('name', self.name)}", f"seed={self.seed}"]
        if self.cut is not None:
            words.append(f"cut={self.cut}")
        if self.pick is not None:
            for value, names in zip(self.pick, PICKS, strict=True):
                if value not in names:
                    raise ScenarioError(f"unknown pick {value}")
            words.append("pick=" + ",".join(self.pick))
        for key, value in self.extra.items():
            if key in RESERVED:
                raise ScenarioError(f"extra key {key} is reserved")
            words.append(f"{_word('key', key)}={_word(key, value)}")
        return " ".join(words)

    def replay(self) -> str:
        return f"FAULTLAB_SCENARIO='{self.line()}'"

    def selected(self) -> bool:
        """Every case without FAULTLAB_SCENARIO; else only the one it names (name, seed, and cut and pick if given)."""
        want = requested()
        if want is None:
            return True
        return (
            (want.name, want.seed) == (self.name, self.seed)
            and (want.cut is None or want.cut == self.cut)
            and (want.pick is None or want.pick == self.pick)
        )


def parse(line: str) -> Scenario:
    words = line.split()
    if not words or words[0] != VERSION:
        raise ScenarioError(f"not a {VERSION} line")
    seen: dict[str, str] = {}
    for word in words[1:]:
        key, eq, value = word.partition("=")
        if not eq or not key:
            raise ScenarioError(f"not key=value: {word}")
        if not value:
            raise ScenarioError(f"empty {key}")
        if key in seen:
            raise ScenarioError(f"{key} twice")
        seen[key] = value
    if "name" not in seen or "seed" not in seen:
        raise ScenarioError("name and seed are required")
    pick: tuple[str, str, str, str] | None = None
    if "pick" in seen:
        parts = seen["pick"].split(",")
        if len(parts) != 4:
            raise ScenarioError("pick takes epochs,names,size,sectors")
        for value, names in zip(parts, PICKS, strict=True):
            if value not in names:
                raise ScenarioError(f"unknown pick {value}")
        pick = (parts[0], parts[1], parts[2], parts[3])
    return Scenario(
        seen["name"],
        _number("seed", seen["seed"]),
        _number("cut", seen["cut"]) if "cut" in seen else None,
        pick,
        {key: value for key, value in seen.items() if key not in RESERVED},
    )


def requested() -> Scenario | None:
    """The case FAULTLAB_SCENARIO names, when it is set."""
    line = os.environ.get("FAULTLAB_SCENARIO", "")
    return parse(line) if line else None
