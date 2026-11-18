#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Germán Méndez Bravo (Kronuz)

"""The Python scenario line against the C++ one's cases: printing, parsing, refusals and selection.

python3 -I tests/scenario_test.py
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from faultlab.scenario import Scenario, ScenarioError, parse

LINE = "faultlab/1 name=mirror_crash.first seed=11 cut=513 pick=all,additions,current,old store=relay"


def refused(line: str) -> bool:
    try:
        parse(line)
    except ScenarioError:
        return True
    return False


def main() -> int:
    results: list[tuple[str, bool]] = []
    s = Scenario("mirror_crash.first", 11, 513, ("all", "additions", "current", "old"), {"store": "relay"})
    results.append(("a line prints as scenario.h prints it", s.line() == LINE))
    results.append(("and parses back", parse(LINE) == s))
    results.append(
        ("whitespace separates words", parse(" faultlab/1\tseed=18446744073709551615 name=x ").seed == 2**64 - 1)
    )
    results.append(("the seed drives one stream", s.random().random() == Scenario("x", 11).random().random()))
    bad = [
        "",
        "faultlab/2 name=x seed=1",
        "faultlab/1 seed=1",
        "faultlab/1 name=x",
        "faultlab/1 name=x seed=-1",
        "faultlab/1 name=x seed=18446744073709551616",
        "faultlab/1 name=x seed=1 seed=2",
        "faultlab/1 name=x seed=1 pick=all,all,all",
        "faultlab/1 name=x seed=1 pick=all,all,all,new",
        "faultlab/1 name=x seed=1 loose",
    ]
    results.append(("malformed lines are refused", all(refused(line) for line in bad)))
    os.environ.pop("FAULTLAB_SCENARIO", None)
    a, b = Scenario("case", 1), Scenario("case", 2, cut=7)
    results.append(("without FAULTLAB_SCENARIO every case runs", a.selected() and b.selected()))
    os.environ["FAULTLAB_SCENARIO"] = "faultlab/1 name=case seed=2"
    results.append(("FAULTLAB_SCENARIO runs exactly its case", not a.selected() and b.selected()))
    os.environ["FAULTLAB_SCENARIO"] = "faultlab/1 name=case seed=2 cut=8"
    results.append(("a named cut must match", not b.selected()))
    os.environ.pop("FAULTLAB_SCENARIO")
    for label, ok in results:
        print(f"  {'ok:  ' if ok else 'FAIL:'} {label}")
    failures = sum(not ok for _, ok in results)
    print(f"=== VERDICT: {'GREEN' if not failures else f'RED ({failures} failures)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
