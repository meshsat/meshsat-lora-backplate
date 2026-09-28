#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Watch the node's radio, and say plainly what a person has to do.

The LoRa back cover's radio can stop answering, and nothing in software resets it: the board
has no reset line and the phone cannot cut the cover's supply. When that happens the daemon
prints `LoRa back cover does not answer on /dev/i2c-5` and exits, systemd tries five times
and gives up. This script runs from a timer every minute and does three things:

- says what the daemon's last words mean, in /run/meshsat-node/status and in the journal,
  once per change: "re-seat the cover", "the cover is not on the bus", or "running";
- while the daemon is stopped, asks the radio once a minute whether it answers again
  (`lora-listen --seconds 1`, which shares the bus with nobody then), and starts the daemon
  when it does, so a re-seated cover comes back without a command;
- while the daemon runs, counts receive errors and recoveries of the last ten minutes and
  notes an unstable radio, without acting on it.

It never claims to have reset the radio. Commands are run through one function so the
decisions can be tested without systemd.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

UNIT = "meshtasticd"
STATUS_PATH = "/run/meshsat-node/status"
CONFIG_D = "/etc/meshtasticd/config.d"
PROBES = ("/usr/lib/meshsat/bin/lora-listen", "/usr/local/lib/meshsat/bin/lora-listen")

NOT_ANSWERING = re.compile(r"LoRa back cover does not answer on (\S+)(.*)$")
UNREACHABLE = re.compile(r"LoRa back cover: (.*)$")
LINED_UP = re.compile(r"LoRa back cover on (\S+) at 0x[0-9a-fA-F]+, bridge lined up after (\d+) reads")
RECOVERING = re.compile(r"LoRa error detected, recovering")
BAD_PACKET = re.compile(r"Ignore rx packet, error=(-?\d+)")
DEVICE = re.compile(r"^\s*I2CDevice:\s*(\S+)")


def classify(lines):
    """What the daemon's last words about the radio mean: (kind, device, message).
    Newest line last. Kinds: ok, radio-not-answering, cover-unreachable, unknown."""
    for line in reversed(list(lines)):
        found = NOT_ANSWERING.search(line)
        if found:
            device, why = found.group(1), found.group(2).strip().strip("()")
            if why:
                return "cover-unreachable", device, f"the back cover does not answer on {device} ({why}): check that it is seated"
            return ("radio-not-answering", device,
                    f"the radio in the back cover does not answer on {device}: take the cover off and press it back on until it clicks")
        found = UNREACHABLE.search(line)
        if found:
            return "cover-unreachable", None, f"the back cover cannot be opened: {found.group(1)}"
        found = LINED_UP.search(line)
        if found:
            return "ok", found.group(1), f"the radio answered when the daemon started (bridge lined up after {found.group(2)} reads)"
    return "unknown", None, "the daemon's log says nothing about the radio"


def storm(lines):
    """Recoveries and refused packets in the lines given (the last ten minutes of the journal)."""
    recoveries = sum(1 for line in lines if RECOVERING.search(line))
    bad = sum(1 for line in lines if BAD_PACKET.search(line))
    return recoveries, bad


def device_from_config(config_d=CONFIG_D):
    try:
        for name in sorted(os.listdir(config_d)):
            if not name.endswith((".yaml", ".yml")):
                continue
            with open(os.path.join(config_d, name), encoding="utf-8") as handle:
                for line in handle:
                    found = DEVICE.match(line)
                    if found:
                        return found.group(1)
    except OSError:
        pass
    return "/dev/i2c-5"


def run(command, timeout=60):
    """(exit code, output) of a command; a missing command or a timeout is an exit code too."""
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return done.returncode, done.stdout + done.stderr
    except FileNotFoundError:
        return 127, f"{command[0]}: not found"
    except subprocess.TimeoutExpired:
        return 124, f"{command[0]}: timed out after {timeout} s"


def read_status(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def write_status(path, status):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(status, handle, indent=1, sort_keys=True)
        handle.write("\n")
    os.replace(tmp, path)


def notify(message, runner=run):
    """A desktop notification for every user with a session bus, best effort, never required."""
    if runner(["which", "notify-send"])[0] != 0:
        return
    try:
        users = os.listdir("/run/user")
    except OSError:
        return
    for uid in users:
        if not uid.isdigit():
            continue
        runner(["sudo", "-u", f"#{uid}", "env", f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus",
                "notify-send", "-a", "MeshSat", "MeshSat node", message], timeout=10)


def decide(unit_state, tail, recent, probe, runner=run, unit=UNIT):
    """The verdict and the action for this minute. `probe` is the alive-check command, or None."""
    now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    status = {"checked_at": now, "daemon": unit_state, "action": "none"}
    if unit_state in ("active", "activating", "reloading"):
        recoveries, bad = storm(recent)
        status.update(recoveries_10min=recoveries, refused_packets_10min=bad)
        if recoveries >= 3 or bad >= 20:
            status.update(radio="unstable", message=f"the radio is running but unstable: {recoveries} recoveries and {bad} refused packets in ten minutes")
        else:
            status.update(radio="ok", message="the node is running")
        return status
    kind, device, message = classify(tail)
    status.update(radio=kind, message=message, device=device)
    if kind in ("radio-not-answering", "unknown") and probe:
        code, output = runner(probe, timeout=45)
        status["probe"] = "answers" if code == 0 else f"no answer (exit {code})"
        if code == 0:
            runner(["systemctl", "reset-failed", unit])
            started, _ = runner(["systemctl", "start", unit])
            if started == 0:
                status.update(radio="recovered", action="started", message="the radio answers again: the node was started")
            else:
                status.update(action="start-failed", message="the radio answers, but the daemon could not be started: see journalctl -u " + unit)
    return status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--status", default=STATUS_PATH)
    parser.add_argument("--unit", default=UNIT)
    parser.add_argument("--probe", help="the alive check; default: the first lora-listen found", default=None)
    parser.add_argument("--no-notify", action="store_true")
    args = parser.parse_args(argv)

    code, out = run(["systemctl", "is-active", args.unit])
    unit_state = out.strip().splitlines()[0] if out.strip() else "unknown"
    _, tail = run(["journalctl", "-u", args.unit, "-n", "60", "--no-pager", "-o", "cat"])
    _, recent = run(["journalctl", "-u", args.unit, "-S", "-10min", "--no-pager", "-o", "cat"])

    probe = None
    probe_binary = args.probe or next((p for p in PROBES if os.access(p, os.X_OK)), None)
    if probe_binary:
        probe = [probe_binary, device_from_config(), "--seconds", "1"]

    before = read_status(args.status)
    status = decide(unit_state, tail.splitlines(), recent.splitlines(), probe)
    write_status(args.status, status)

    changed = (before.get("radio"), before.get("message")) != (status["radio"], status["message"])
    if changed or status["action"] != "none":
        print(f"meshsat-radio-watch: daemon {status['daemon']}, radio {status['radio']}: {status['message']}")
        if status["radio"] in ("radio-not-answering", "cover-unreachable") and not args.no_notify:
            notify(status["message"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
