#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The airtime sum and the reading of the daemon's log, against figures the radio library gave."""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest

import daemon_airtime

LOG = """=== 2026-09-27 22:19:52 start
INFO  | 22:19:53 0.515 Slot time: 28 msec, preamble time: 1310 msec
DEBUG | 22:20:27 34.956 [RadioIf] Started Tx (id=0x03a06b55 fr=0x52cb81e7 to=0xffffffff, transport = 0, WantAck=0, HopLim=3 Ch=206 encrypted len=176 rxtime=1790540423 hopStart=3 relay=0xe7 priority=10)
DEBUG | 22:20:30 37.716 [RadioIf] Completed sending (id=0x03a06b55 fr=0x52cb81e7 to=0xffffffff, transport = 0, WantAck=0, HopLim=3 Ch=206 encrypted len=176 rxtime=1790540423 hopStart=3 relay=0xe7 priority=10)
=== 2026-09-27 23:59:40 start
INFO  | 23:59:41 0.262 Slot time: 28 msec, preamble time: 131 msec
DEBUG | 23:59:58 18.105 [RadioIf] Completed sending (id=0xa9d56df2 fr=0x52cb81e7 to=0xffffffff, transport = 0, WantAck=0, HopLim=3 Ch=206 encrypted len=90 hopStart=3 relay=0xe7)
DEBUG | 00:00:06 26.411 [RadioIf] Completed sending (id=0xa9d56df2 fr=0x52cb81e7 to=0xffffffff, transport = 0, WantAck=0, HopLim=3 Ch=206 encrypted len=90 hopStart=3 relay=0xe7)
"""


class Airtime(unittest.TestCase):
    def test_the_sum_is_the_radio_librarys(self):
        # bytes handed to the radio, preamble symbols, what RadioLib 7.7.1 said on the bench
        for length, preamble, said in ((32, 16, 477), (76, 16, 805), (126, 16, 1174), (192, 16, 1665), (126, 160, 2353),
                                       (192, 160, 2845), (126, 200, 2681), (192, 200, 3172), (237, 200, 3541), (192, 320, 4155)):
            self.assertAlmostEqual(daemon_airtime.airtime_ms(length, preamble), said, delta=1)

    def test_every_sending_of_the_daemon_is_a_frame(self):
        frames = list(daemon_airtime.frames(LOG.splitlines()))
        self.assertEqual([f["length"] for f in frames], [176, 90, 90])
        self.assertEqual([f["preamble"] for f in frames], [160, 16, 16])
        self.assertEqual([f["airtime_ms"] for f in frames], [2722, 928, 928])
        self.assertEqual(frames[0]["sender"], "0x52cb81e7")

    def test_a_log_that_runs_past_midnight(self):
        frames = list(daemon_airtime.frames(LOG.splitlines()))
        self.assertAlmostEqual(frames[2]["t"] - frames[1]["t"], 8, delta=0.5)

    def test_a_log_without_a_date_needs_one(self):
        lines = [line for line in LOG.splitlines() if not line.startswith("===")]
        with self.assertRaises(ValueError):
            list(daemon_airtime.frames(lines))
        self.assertEqual(len(list(daemon_airtime.frames(lines, date="2026-09-27"))), 3)


class Capture(unittest.TestCase):
    """capture.py on a pseudo-terminal that stands in for the receiver's serial port."""

    def setUp(self):
        self.master, slave = os.openpty()
        self.port = os.ttyname(slave)
        os.close(slave)
        self.folder = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.folder.name, "receiver.jsonl")
        here = os.path.dirname(os.path.abspath(__file__))
        self.tool = [sys.executable, os.path.join(here, "capture.py")]
        self.capture = subprocess.Popen(self.tool + [self.port, "--out", self.out, "--beat", "0.2"])
        for _ in range(100):
            if os.path.exists(self.out) and os.path.getsize(self.out):
                break
            time.sleep(0.05)

    def tearDown(self):
        if self.capture.poll() is None:
            self.capture.terminate()
            self.capture.wait(timeout=5)
        os.close(self.master)
        self.folder.cleanup()

    def records(self):
        with open(self.out, encoding="utf-8") as handle:
            return [json.loads(line) for line in handle]

    def test_lines_are_kept_with_the_time_they_were_read(self):
        before = time.time()
        os.write(self.master, b"\x1b[34mDEBUG \x1b[0m| 16:00:00 1000 [RadioIf] Packet RX: 477ms\r\nhalf a li")
        time.sleep(0.6)
        os.write(self.master, b"ne\r\n")
        time.sleep(0.6)
        lines = [r for r in self.records() if r["kind"] == "line"]
        self.assertEqual([r["text"] for r in lines], ["DEBUG | 16:00:00 1000 [RadioIf] Packet RX: 477ms", "half a line"])
        self.assertTrue(before <= lines[0]["wall"] <= time.time())
        self.assertLess(lines[0]["mono"], lines[1]["mono"])
        self.assertGreaterEqual(len([r for r in self.records() if r["kind"] == "heartbeat"]), 3)

    def test_check_tells_a_silent_receiver_from_a_talking_one(self):
        time.sleep(0.8)
        silent = subprocess.run(self.tool + ["--check", self.out, "--fresh", "5"], capture_output=True, text=True)
        self.assertEqual(silent.returncode, 1)
        self.assertIn("capture running, receiver SILENT", silent.stdout)
        os.write(self.master, b"INFO  | 16:00:01 1001 [Power] Battery: usbPower=1\n")
        time.sleep(0.6)
        talking = subprocess.run(self.tool + ["--check", self.out, "--fresh", "5"], capture_output=True, text=True)
        self.assertEqual(talking.returncode, 0)
        self.assertIn("capture running, receiver talking", talking.stdout)

    def test_a_second_capture_into_the_same_file_is_refused(self):
        second = subprocess.run(self.tool + [self.port, "--out", self.out], capture_output=True, text=True, timeout=10)
        self.assertEqual(second.returncode, 1)
        self.assertIn("running already", second.stderr)

    def test_a_capture_that_was_stopped_says_so_and_is_not_running(self):
        self.capture.terminate()
        self.capture.wait(timeout=5)
        self.assertEqual(self.records()[-1]["kind"], "stop")
        gone = subprocess.run(self.tool + ["--check", self.out, "--fresh", "0.1"], capture_output=True, text=True)
        time.sleep(0.2)
        gone = subprocess.run(self.tool + ["--check", self.out, "--fresh", "0.1"], capture_output=True, text=True)
        self.assertEqual(gone.returncode, 1)
        self.assertIn("capture NOT running", gone.stdout)

    def test_no_capture_file_is_no_capture(self):
        none = subprocess.run(self.tool + ["--check", os.path.join(self.folder.name, "none.jsonl")], capture_output=True, text=True)
        self.assertEqual(none.returncode, 1)


if __name__ == "__main__":
    unittest.main()
