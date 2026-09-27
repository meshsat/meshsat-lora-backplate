#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep what a receiving node writes on its serial port, in a way a verdict can lean on.

Every line is written with the time this computer read it, on a clock that does not jump
and on the wall clock. Every few seconds the capture writes a line of its own, so that a
receiver with nothing to say can be told from a capture that was not running. One capture
per port: a second one would take every other byte from the first.

    capture.py /dev/ttyACM0 --out receiver.jsonl        # start this before anything is sent
    capture.py --check receiver.jsonl                    # is it running, and is the receiver talking?

Opening the port restarts some nodes. Start the capture, wait for --check to say the
receiver is talking, then send.
"""
import argparse
import json
import os
import re
import signal
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def write(handle, record: dict) -> None:
    handle.write(json.dumps(record, ensure_ascii=False) + "\n")
    handle.flush()
    os.fsync(handle.fileno())


def check(path: str, fresh: float) -> int:
    """0 when the capture ran and the receiver spoke within `fresh` seconds, 1 otherwise."""
    beat = spoke = None
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if record.get("kind") == "heartbeat":
                    beat = record["wall"]
                elif record.get("kind") == "line":
                    spoke = record["wall"]
    except OSError as error:
        print(f"no capture: {error}")
        return 1
    now = time.time()
    running = beat is not None and now - beat <= fresh
    talking = spoke is not None and now - spoke <= fresh
    print(f"capture {'running' if running else 'NOT running'}, receiver {'talking' if talking else 'SILENT'}"
          f" (last sign of the capture {'never' if beat is None else f'{now - beat:.0f} s ago'},"
          f" last line of the receiver {'never' if spoke is None else f'{now - spoke:.0f} s ago'})")
    return 0 if running and talking else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("port", nargs="?", help="the receiver's serial port")
    parser.add_argument("--out", help="the file to append to")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--beat", type=float, default=5.0, help="seconds between the capture's own lines")
    parser.add_argument("--check", metavar="FILE", help="say whether that capture is running, and leave")
    parser.add_argument("--fresh", type=float, default=60.0, help="for --check: how old the last line may be")
    args = parser.parse_args()
    if args.check:
        return check(args.check, args.fresh)
    if not args.port or not args.out:
        parser.error("a port and --out, or --check")

    import fcntl
    import termios

    lock = open(args.out + ".lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print(f"a capture into {args.out} is running already", file=sys.stderr)
        return 1
    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print(f"{args.port} is held by another program", file=sys.stderr)
        return 1
    speed = getattr(termios, f"B{args.baud}")
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0  # raw: nothing translated, nothing echoed
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[4] = attrs[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    stopping = []
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, lambda *_: stopping.append(True))

    import select

    with open(args.out, "a", encoding="utf-8") as out:
        write(out, {"kind": "start", "wall": time.time(), "mono": time.monotonic(), "port": args.port, "pid": os.getpid()})
        pending = b""
        beat = time.monotonic()
        write(out, {"kind": "heartbeat", "wall": time.time(), "mono": beat})
        while not stopping:
            ready, _, _ = select.select([fd], [], [], min(0.5, args.beat / 2))
            now_mono, now_wall = time.monotonic(), time.time()
            if ready:
                try:
                    chunk = os.read(fd, 4096)
                except BlockingIOError:
                    chunk = b""
                except OSError as error:
                    write(out, {"kind": "lost", "wall": now_wall, "mono": now_mono, "why": str(error)})
                    return 1
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    text = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip("\r")
                    if text:
                        write(out, {"kind": "line", "wall": now_wall, "mono": now_mono, "text": text})
            if now_mono - beat >= args.beat:
                beat = now_mono
                write(out, {"kind": "heartbeat", "wall": now_wall, "mono": now_mono})
        write(out, {"kind": "stop", "wall": time.time(), "mono": time.monotonic()})
    return 0


if __name__ == "__main__":
    sys.exit(main())
