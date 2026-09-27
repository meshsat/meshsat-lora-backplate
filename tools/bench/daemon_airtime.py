#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put what meshtasticd sent into the file that keeps the airtime of the hour.

lora-ping writes its own frames there. The daemon does not, so its log is read: every
`Completed sending` line becomes a frame in the ledger, with the time on air of its length
and of the preamble the daemon said it uses. A frame that is in the ledger already is not
added again, so the log can be read as often as wanted.

    daemon_airtime.py meshtasticd.log --ledger ~/.local/state/meshsat-lora-backplate/airtime.jsonl

The log's lines carry a time and no date. The date is taken from the `=== 2026-09-27 19:49:40
start` lines the bench runner writes, or from --date.
"""
import argparse
import datetime
import json
import math
import os
import re
import sys

START = re.compile(r"^=== (\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d) start")
PREAMBLE = re.compile(r"preamble time: (\d+) msec")
SENT = re.compile(
    r"\| (\d\d:\d\d:\d\d) [\d.]+ \[RadioIf\] Completed sending \(id=(0x[0-9a-f]+) fr=(0x[0-9a-f]+) to=(0x[0-9a-f]+),"
    r".*\bencrypted len=(\d+)"
)


def airtime_ms(length: int, preamble: int, sf: int = 11, bw_khz: float = 250.0, cr: int = 5) -> float:
    """Time on air of a LoRa frame with an explicit header and a checksum, in milliseconds.
    `length` is what the radio is handed: Meshtastic's 16-byte header and the payload."""
    symbol = (1 << sf) / bw_khz
    low_rate = 1 if symbol >= 16.0 else 0
    payload = 8 + max(math.ceil((8 * length - 4 * sf + 28 + 16) / (4 * (sf - 2 * low_rate))) * cr, 0)
    return (preamble + 4.25 + payload) * symbol


def frames(lines, date=None, sf: int = 11, bw_khz: float = 250.0):
    """One dict for every frame the log says was sent."""
    symbol = (1 << sf) / bw_khz
    day = datetime.date.fromisoformat(date) if date else None
    preamble, last = 16, None
    for line in lines:
        started = START.match(line)
        if started:
            day = datetime.date.fromisoformat(started.group(1))
            last = None
            continue
        told = PREAMBLE.search(line)
        if told:
            preamble = round(int(told.group(1)) / symbol)
            continue
        sent = SENT.search(line)
        if not sent:
            continue
        if day is None:
            raise ValueError("no date: the log has no start line, give --date")
        clock = datetime.time.fromisoformat(sent.group(1))
        if last is not None and clock < last:
            day += datetime.timedelta(days=1)  # the log ran past midnight
        last = clock
        when = datetime.datetime.combine(day, clock).astimezone()
        length = int(sent.group(5))
        yield {
            "t": when.timestamp(),
            "event": "attempt",
            "tool": "meshtasticd",
            "id": sent.group(2),
            "sender": sent.group(3),
            "to": sent.group(4),
            "length": length,
            "preamble": preamble,
            "airtime_ms": round(airtime_ms(length, preamble, sf, bw_khz)),
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", nargs="+")
    parser.add_argument("--ledger", required=True)
    parser.add_argument("--date", help="the day the log begins, as 2026-09-27, when the log does not say")
    args = parser.parse_args()

    known = set()
    if os.path.exists(args.ledger):
        with open(args.ledger, encoding="utf-8") as handle:
            for line in handle:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if record.get("tool") == "meshtasticd":
                    known.add((record.get("id"), round(record.get("t", 0))))
    added = 0
    os.makedirs(os.path.dirname(os.path.abspath(args.ledger)), exist_ok=True)
    with open(args.ledger, "a", encoding="utf-8") as out:
        for path in args.log:
            with open(path, encoding="utf-8", errors="replace") as handle:
                for frame in frames(handle, args.date):
                    key = (frame["id"], round(frame["t"]))
                    if key in known:
                        continue
                    known.add(key)
                    out.write(json.dumps(frame) + "\n")
                    added += 1
        out.flush()
        os.fsync(out.fileno())
    print(f"{added} frames of the daemon added to {args.ledger}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
