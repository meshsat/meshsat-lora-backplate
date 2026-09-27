#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read a paired comparison the way it was designed: block by block, not arm by arm.

Each block holds one frame of each arm under the same preparation. What separates the arms
is the blocks in which they disagree; the blocks in which both were accepted or both refused
say nothing about the difference. The exact one-sided probability below is that of seeing at
least this many disagreements in favour of the intervention if the arms were the same (a
binomial with p = 1/2 over the disagreeing blocks). It assumes the blocks are independent,
which serial radio trials on one warming board may not be; it is a screen, not a proof.

    pairs.py ledger.jsonl --intervention B --control C [--block-from label]

The ledger is verdict.py's output. A row's arm and block are read from its label, written
by the run as <run>-b<block>[r]-<arm>; a repeated block replaces the original only when the
original was void.
"""
import argparse
import json
import math
import re
import sys

LABEL = re.compile(r"-b(\d+)(r?)-([A-Za-z]+)$")


def one_sided_p(favour: int, against: int) -> float:
    """P(at least `favour` of the disagreeing blocks favour the intervention), arms being equal."""
    n = favour + against
    if n == 0:
        return 1.0
    return sum(math.comb(n, k) for k in range(favour, n + 1)) / 2**n


def read_pairs(rows, intervention: str, control: str) -> dict:
    """{block: {arm: verdict}} with a repeat used only where the original frame was void."""
    blocks = {}
    for row in rows:
        found = LABEL.search(row.get("label") or row.get("attempt") or "")
        if not found:
            continue
        block, repeat, arm = int(found.group(1)), bool(found.group(2)), found.group(3)
        if arm not in (intervention, control):
            continue
        slot = blocks.setdefault(block, {})
        void = row.get("void", False) or row.get("outcome", {}) and row["outcome"].get("mode_loaded") not in (None, row.get("mode_expected", row["outcome"].get("mode_loaded")))
        if repeat:
            if slot.get(arm, {}).get("void", True):
                slot[arm] = {"verdict": row["verdict"], "void": bool(void), "repeat": True}
        else:
            slot[arm] = {"verdict": row["verdict"], "void": bool(void), "repeat": False}
    return blocks


def summarise(blocks: dict, intervention: str, control: str) -> dict:
    accepted = {arm: 0 for arm in (intervention, control)}
    judged = {arm: 0 for arm in (intervention, control)}
    favour = against = both = neither = incomplete = 0
    for block, arms in sorted(blocks.items()):
        pair = [arms.get(arm) for arm in (intervention, control)]
        for arm, entry in zip((intervention, control), pair):
            if entry and not entry["void"] and entry["verdict"] in ("accepted", "crc_failed", "radio_error", "not_observed"):
                judged[arm] += 1
                accepted[arm] += entry["verdict"] == "accepted"
        if any(e is None or e["void"] or e["verdict"] not in ("accepted", "crc_failed", "radio_error", "not_observed") for e in pair):
            incomplete += 1
            continue
        b_ok, c_ok = pair[0]["verdict"] == "accepted", pair[1]["verdict"] == "accepted"
        if b_ok and not c_ok:
            favour += 1
        elif c_ok and not b_ok:
            against += 1
        elif b_ok:
            both += 1
        else:
            neither += 1
    return {
        "blocks": len(blocks),
        "complete_pairs": both + neither + favour + against,
        "incomplete_pairs": incomplete,
        "accepted": accepted,
        "judged": judged,
        "both_accepted": both,
        "neither_accepted": neither,
        f"only_{intervention}_accepted": favour,
        f"only_{control}_accepted": against,
        "one_sided_p_if_arms_equal": round(one_sided_p(favour, against), 4),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ledger")
    parser.add_argument("--intervention", default="B")
    parser.add_argument("--control", default="C")
    args = parser.parse_args()
    with open(args.ledger, encoding="utf-8") as handle:
        rows = [json.loads(line) for line in handle if line.strip()]
    blocks = read_pairs(rows, args.intervention, args.control)
    for block, arms in sorted(blocks.items()):
        print(f"block {block}: " + ", ".join(f"{arm} {e['verdict']}{' (void)' if e['void'] else ''}{' (repeat)' if e['repeat'] else ''}" for arm, e in sorted(arms.items())))
    print(json.dumps(summarise(blocks, args.intervention, args.control), indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
