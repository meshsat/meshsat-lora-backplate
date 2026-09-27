#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Judge transmitted frames by what a receiving Meshtastic node wrote in its firmware log.

A frame is accepted only on positive evidence: a complete `Lora RX` line of the receiver
that carries the frame's packet id, its sender and its length, at the time the frame was
on the air. Everything else is named for what it is. A capture that cannot vouch for the
moment a frame was sent gives no verdict on that frame, neither for the radio nor against.

    accepted         the receiver's checksum passed; says nothing about the content
    crc_failed       heard, refused with error -7; the id in such a line is tentative
    radio_error      heard, refused with another error, which is kept
    not_observed     the capture ran across the frame's time and holds no trace of it
    ambiguous        the log contradicts itself, a line is cut, or the receiver was sending
    capture_invalid  the capture does not cover the frame's time, or cannot be aligned

    verdict.py --attempts attempts.jsonl --log receiver.log [--log more.log] --out ledger.jsonl

attempts.jsonl holds one JSON object per frame sent, with at least "attempt", "id",
"sender" and "length" (the bytes handed to the radio, header included), and for the capture
check "t_end" (seconds on any clock that does not jump) and "airtime_ms". The receiver's
clock is never trusted: its uptime is lined up with the sender's clock through the frames
both sides agree on.

A log is either the receiver's output as it came, or the file capture.py writes. The second
says when this computer read each line and proves, line by line of its own, that the capture
was running while the receiver had nothing to say.

Time. "t_end" is when the sender saw the frame end: the tool stamps it after the radio said
transmit-done, the daemon logs "Completed sending", both some tens of milliseconds after the
last symbol. A receiver writes its line after the last symbol and its own processing. The two
clocks are lined up on the frames both sides agree on (the median of their differences), and a
line counts for a frame when it falls between the frame's start less --tolerance and its end
plus --tolerance, 4 s by default: room for the stamps, the receiver's processing and the
alignment, and too little for the same id logged at another time. The tolerance is not widened
until a match appears.

Identity. A match needs the packet id, the sender and the length. An attempt whose sender was
not written down on the sender side ("sender_recorded": false, the sender then taken from the
receiver's own line) is matched on id and length, and its row says so in "identity".
"""
import argparse
import json
import re
import statistics
import sys
from dataclasses import dataclass, field

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# A log line can follow binary output of the node's client interface on the same line. Firmware
# before 2.7 writes no thread name in brackets; the packet lines themselves read the same.
LINE = re.compile(r"(DEBUG|INFO|WARN|ERROR|TRACE|CRIT)\s*\|\s*(\S+)\s+(\d+)\s+(?:\[([^\]]+)\]\s+)?(.*)$")
RX_OK = re.compile(r"^Lora RX \(id=(0x[0-9a-f]+) fr=(0x[0-9a-f]+) to=(0x[0-9a-f]+),.*\bencrypted len=(\d+)\b(.*)\)$")
RX_REFUSED = re.compile(
    r"^Ignore (?:received packet due to error=|rx packet, error=)(-?\d+) "
    r"\(maybe id=(0x[0-9a-f]+) fr=(0x[0-9a-f]+) to=(0x[0-9a-f]+)\b(.*)\)$"
)
RX_BEGUN = re.compile(r"^(?:Lora RX|Ignore (?:received packet|rx packet))")
OWN_TX_START = re.compile(r"^Started Tx \(id=(0x[0-9a-f]+)")
OWN_TX_END = re.compile(r"^Completed sending \(id=(0x[0-9a-f]+)")
OFFSET = re.compile(r"^Corrected frequency offset: (-?[\d.]+)$")
# The receiver's own sum of the time a packet of that length takes, written after either line.
RX_TIME = re.compile(r"^Packet RX(?: \(noise\?\))? ?: (\d+)ms$")
SIGNAL = re.compile(r"rxSNR=(-?[\d.]+) rxRSSI=(-?\d+)")
HEX = re.compile(r"0x[0-9a-f]+")

CRC_MISMATCH = -7


@dataclass
class Event:
    kind: str  # accepted, refused, partial, own_tx_start, own_tx_end, other, heartbeat
    source: str
    line: int
    uptime: float  # the receiver's uptime, or the capture's wall clock when the capture gives one
    packet_id: str = ""
    sender: str = ""
    length: int = -1
    error: int = 0
    snr: float | None = None
    rssi: int | None = None
    offset_hz: float | None = None
    rx_time_ms: int | None = None
    ids_seen: list = field(default_factory=list)

    def where(self) -> str:
        return f"{self.source}:{self.line}"


def norm(value) -> str:
    """Packet ids and node numbers as the firmware prints them: 0x and lower case, no padding."""
    if isinstance(value, int):
        return hex(value)
    text = str(value).strip().lower()
    if text.startswith("!"):
        text = "0x" + text[1:]
    return hex(int(text, 16))


def expected_payload(packet_id, length: int) -> bytes:
    """The payload lora-ping gives a frame with this packet id: xorshift32, the top byte of every step."""
    x = int(norm(packet_id), 16) or 0x6D657368
    out = bytearray()
    for _ in range(length):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(x >> 24)
    return bytes(out)


def read_log(path: str, source: str | None = None) -> list:
    """Every line of a receiver log that the firmware wrote, as events in the order written."""
    events = []
    pending_offset = None
    last_packet = None
    with open(path, "rb") as handle:
        text = handle.read().decode("utf-8", errors="replace")
    for number, raw in enumerate(text.splitlines(), start=1):
        read_at = None
        if raw.startswith("{"):
            # A line of capture.py: the receiver's text with the time it was read, or the capture's own sign.
            try:
                record = json.loads(raw)
            except ValueError:
                continue
            if record.get("kind") == "heartbeat":
                events.append(Event("heartbeat", source or path, number, float(record["wall"])))
                continue
            if record.get("kind") != "line":
                continue
            read_at, raw = float(record["wall"]), record.get("text", "")
        found = LINE.search(ANSI.sub("", raw).rstrip("\r"))
        if not found:
            continue
        _, _, uptime, thread, message = found.groups()
        event = Event("other", source or path, number, int(uptime) if read_at is None else read_at)
        uptime = event.uptime
        if thread in ("RadioIf", None):
            offset = OFFSET.match(message)
            accepted = RX_OK.match(message)
            refused = RX_REFUSED.match(message)
            rx_time = RX_TIME.match(message)
            if rx_time and last_packet is not None and abs(last_packet.uptime - uptime) <= 1:
                last_packet.rx_time_ms = int(rx_time.group(1))
                last_packet = None
            elif offset:
                pending_offset = (uptime, float(offset.group(1)))
            elif accepted:
                event.kind = "accepted"
                event.packet_id, event.sender = norm(accepted.group(1)), norm(accepted.group(2))
                event.length = int(accepted.group(4))
                # The firmware writes the offset of a packet just before the packet's own line.
                if pending_offset and abs(pending_offset[0] - event.uptime) <= 1:
                    event.offset_hz = pending_offset[1]
                pending_offset = None
            elif refused:
                event.kind = "refused"
                event.error = int(refused.group(1))
                event.packet_id, event.sender = norm(refused.group(2)), norm(refused.group(3))
            elif RX_BEGUN.match(message):
                # A line about a received packet that does not end as one: cut, or run into another.
                event.kind = "partial"
                event.ids_seen = [norm(x) for x in HEX.findall(message)]
            elif OWN_TX_START.match(message):
                event.kind = "own_tx_start"
                event.packet_id = norm(OWN_TX_START.match(message).group(1))
            elif OWN_TX_END.match(message):
                event.kind = "own_tx_end"
                event.packet_id = norm(OWN_TX_END.match(message).group(1))
            signal = SIGNAL.search(message)
            if signal and event.kind in ("accepted", "refused"):
                event.snr, event.rssi = float(signal.group(1)), int(signal.group(2))
            if event.kind in ("accepted", "refused"):
                last_packet = event
        events.append(event)
    return events


def own_transmissions(events: list) -> list:
    """The spans of receiver uptime in which the receiver itself was sending."""
    spans, open_at = [], {}
    for event in events:
        if event.kind == "own_tx_start":
            open_at[event.packet_id] = event.uptime
        elif event.kind == "own_tx_end" and event.packet_id in open_at:
            spans.append((open_at.pop(event.packet_id), event.uptime))
    # A start without an end is a transmission the capture did not see finish.
    spans.extend((start, start + 10) for start in open_at.values())
    return spans


def judge(attempts: list, events: list, tolerance: float = 4.0, health: float = 60.0) -> list:
    """One verdict per attempt. Nothing is dropped: the result has as many rows as `attempts`."""
    ids = [norm(a["id"]) for a in attempts]
    repeated = {i for i in ids if ids.count(i) > 1}
    # What the receiver wrote vouches for the receiver; the capture's own lines only for the capture.
    uptimes = sorted(e.uptime for e in events if e.kind != "heartbeat")
    beats = sorted(e.uptime for e in events if e.kind == "heartbeat")
    sending = own_transmissions(events)

    by_id = {}
    for event in events:
        if event.kind in ("accepted", "refused"):
            by_id.setdefault(event.packet_id, []).append(event)

    # Line the two clocks up on the frames both sides know, the complete lines only.
    deltas = []
    for attempt in attempts:
        if "t_end" not in attempt or norm(attempt["id"]) in repeated:
            continue
        for event in by_id.get(norm(attempt["id"]), []):
            if event.sender == norm(attempt["sender"]):
                deltas.append(float(attempt["t_end"]) - event.uptime)
    shift = statistics.median(deltas) if deltas else None

    def window(attempt):
        if shift is None or "t_end" not in attempt:
            return None
        end = float(attempt["t_end"]) - shift
        return end - float(attempt.get("airtime_ms", 0)) / 1000.0 - tolerance, end + tolerance

    def covered(span, times=None) -> bool:
        times = uptimes if times is None else times
        before = any(span[0] - health <= u <= span[0] for u in times)
        after = any(span[1] <= u <= span[1] + health for u in times)
        return before and after

    rows = []
    for attempt in attempts:
        packet_id, sender = norm(attempt["id"]), norm(attempt["sender"])
        span = window(attempt)
        row = dict(attempt)
        row.update(verdict="", reason="", content="not_checked", events=[], error_codes=[], capture="unknown")
        row["identity"] = "id, sender, length" if attempt.get("sender_recorded", True) else "id, length; the sender is the receiver's"

        def inside(event) -> bool:
            return span is None or span[0] <= event.uptime <= span[1]

        matches = [e for e in by_id.get(packet_id, [])]
        ours = [e for e in matches if e.kind == "accepted" and e.sender == sender]
        refused = [e for e in matches if e.kind == "refused"]
        strangers = [e for e in matches if e.kind == "accepted" and e.sender != sender]
        cut = [e for e in events if e.kind == "partial" and packet_id in e.ids_seen]
        # What the receiver heard in the frame's time and could not be given to any attempt.
        others_in_span = (
            [
                e
                for e in events
                if span[0] <= e.uptime <= span[1]
                and ((e.kind == "refused" and e.packet_id not in ids) or (e.kind == "partial" and e.ids_seen))
            ]
            if span
            else []
        )
        row["events"] = [e.where() for e in ours + refused + strangers + cut]
        row["error_codes"] = sorted({e.error for e in refused})
        if span:
            row["capture"] = "covers" if covered(span) else "does_not_cover"
            row["receiver_sending"] = any(s[0] <= span[1] and span[0] <= s[1] for s in sending)

        def settle(verdict, reason, event=None):
            row["verdict"], row["reason"] = verdict, reason
            if event is not None:
                row.update(rx_len=event.length, snr=event.snr, rssi=event.rssi, offset_hz=event.offset_hz)
                row["rx_time_ms"] = event.rx_time_ms
            rows.append(row)

        if packet_id in repeated:
            settle("ambiguous", "more than one attempt carries this packet id")
        elif cut:
            settle("ambiguous", "a line about this packet is cut or run into another")
        elif ours and refused:
            settle("ambiguous", "the receiver both accepted and refused this packet id")
        elif strangers and not ours:
            settle("ambiguous", "accepted with this packet id from another sender")
        elif ours:
            lengths = {e.length for e in ours}
            if lengths != {int(attempt["length"])}:
                settle("ambiguous", f"accepted with length {sorted(lengths)}, sent {attempt['length']}", ours[0])
            elif not all(inside(e) for e in ours):
                settle("ambiguous", "accepted outside the time the frame was on the air", ours[0])
            else:
                settle("accepted", "complete Lora RX line with id, sender and length", ours[0])
        elif refused:
            event = refused[0]
            if not all(inside(e) for e in refused):
                settle("ambiguous", "refused outside the time the frame was on the air", event)
            elif set(row["error_codes"]) == {CRC_MISMATCH}:
                note = "" if event.sender == sender else ", sender field differs"
                settle("crc_failed", "checksum failure, id tentative" + note, event)
            else:
                settle("radio_error", f"refused with error {row['error_codes']}", event)
        elif span is None:
            settle("capture_invalid", "no frame of this run lines the capture up, or the attempt has no time")
        elif row["capture"] != "covers" and covered(span, beats):
            row["capture"] = "receiver_silent"
            settle("capture_invalid", "the capture ran, and the receiver wrote nothing around the time the frame was on the air")
        elif row["capture"] != "covers":
            settle("capture_invalid", "the capture holds no line around the time the frame was on the air")
        elif row.get("receiver_sending"):
            settle("ambiguous", "the receiver was sending while the frame was on the air")
        elif others_in_span:
            row["events"] = [e.where() for e in others_in_span]
            settle("ambiguous", "a refused or cut packet line with another id fell into the frame's time")
        else:
            settle("not_observed", "the capture covers the frame's time and holds no trace of it")
    return rows


def load_attempts(path: str) -> list:
    """The frames to judge, from a file of attempts or from the file lora-ping writes its frames into.
    There a frame is an `attempt` line written before it was sent and an `outcome` line after."""
    with open(path, encoding="utf-8") as handle:
        records = [json.loads(line) for line in handle if line.strip().startswith("{")]
    if not any("event" in record for record in records):
        return records
    outcomes = {(r.get("run"), r.get("frame")): r for r in records if r.get("event") == "outcome"}
    attempts = []
    for record in records:
        if record.get("event") != "attempt":
            continue
        frame = dict(record)
        outcome = outcomes.get((record.get("run"), record.get("frame")))
        if record.get("tool") == "meshtasticd":
            # The daemon's log gives the moment a frame was over.
            frame["attempt"] = f"daemon-{record['id']}-{record['t']:.0f}"
            frame["t_end"] = record["t"]
        else:
            frame["attempt"] = f"{record.get('run')}-{record.get('frame')}"
            # Written down before the load; over when the outcome was written, or after load and airtime.
            frame["t_end"] = outcome["t"] if outcome else record["t"] + (record["airtime_ms"] + 1500.0) / 1000.0
            frame["outcome"] = outcome
        attempts.append(frame)
    return attempts


def summary(rows: list) -> dict:
    counts = {}
    for row in rows:
        counts[row["verdict"]] = counts.get(row["verdict"], 0) + 1
    counts["sent"] = len(rows)
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--attempts", required=True, help="one JSON object per frame sent, or lora-ping's own file")
    parser.add_argument("--log", action="append", required=True, help="a receiver log; repeat for more")
    parser.add_argument("--out", help="write one JSON object per attempt, with its verdict")
    parser.add_argument("--tolerance", type=float, default=4.0, help="seconds either side of a frame")
    parser.add_argument("--health", type=float, default=60.0, help="a capture covers a frame with a line this close")
    args = parser.parse_args()

    attempts = load_attempts(args.attempts)
    events = []
    for path in args.log:
        events.extend(read_log(path))
    rows = judge(attempts, events, args.tolerance, args.health)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row, sort_keys=True) + "\n")
    counts = summary(rows)
    assert sum(v for k, v in counts.items() if k != "sent") == counts["sent"]
    print(json.dumps(counts, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
