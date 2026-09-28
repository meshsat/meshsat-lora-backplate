#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The watchdog's decisions, on the daemon's words of 27 September 2026 and without systemd."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import meshsat_radio_watch as watch  # noqa: E402

# Verbatim from the bench logs of 27 September 2026 (meshtasticd-2154-rx-duty-cycle.log and
# meshtasticd-2219-no-answer.log): the storm, the recovery, and the radio that stopped answering.
HEALTHY_START = [
    "Portduino is starting, VFS root at /var/lib/meshtasticd/.portduino/default",
    "INFO  | 22:19:53 0.515 Set radio: region=EU_868, name=msat-ttc-01, config=0, ch=0, power=27",
    "LoRa back cover on /dev/i2c-5 at 0x28, bridge lined up after 15 reads",
    "INFO  | 22:19:54 1.428 SX126x init result 0",
    "INFO  | 22:19:54 1.430 sx1262 init success",
]
STORM = [
    "ERROR | 21:48:41 5.275 [RadioIf] Ignore rx packet, error=-706 (maybe id=0x00000000 fr=0x00000000 to=0x00000000 flags=0x00 rxSNR=25.5 rxRSSI=-105 nextHop=0x0 relay=0x0)",
    "WARN  | 21:48:41 5.300 [RadioIf] Ignore received packet too short",
    "DEBUG | 21:48:46 10.355 [RadioIf] SX126x standby RadioLib err=-706",
    "ERROR | 21:48:47 10.568 LoRa error detected, recovering",
    "ERROR | 21:49:11 35.294 [RadioIf] Ignore rx packet, error=-7 (maybe id=0xb1af2dce fr=0x52cb81e7 to=0xffffffff flags=0x63 rxSNR=25.5 rxRSSI=-34 nextHop=0x0 relay=0xe7)",
    "ERROR | 21:49:12 36.100 LoRa error detected, recovering",
    "ERROR | 21:50:10 94.006 [RadioIf] SX126X startReceiveDutyCycleAuto RadioLib err=-1",
    "ERROR | 21:50:10 94.024 LoRa error detected, recovering",
]
NOT_ANSWERING = ["LoRa back cover does not answer on /dev/i2c-5"]
UNREACHABLE_BUS = ["LoRa back cover does not answer on /dev/i2c-5 (i2c write: Remote I/O error)"]
CANNOT_OPEN = ["LoRa back cover: /dev/i2c-5: No such file or directory"]


class Classify(unittest.TestCase):
    def test_the_radio_that_stopped_answering_means_re_seat(self):
        kind, device, message = watch.classify(HEALTHY_START + STORM + NOT_ANSWERING)
        self.assertEqual((kind, device), ("radio-not-answering", "/dev/i2c-5"))
        self.assertIn("take the cover off", message)

    def test_a_bus_error_means_the_cover_is_not_seated(self):
        kind, _, message = watch.classify(HEALTHY_START + UNREACHABLE_BUS)
        self.assertEqual(kind, "cover-unreachable")
        self.assertIn("Remote I/O error", message)

    def test_a_device_that_cannot_be_opened(self):
        kind, _, message = watch.classify(CANNOT_OPEN)
        self.assertEqual(kind, "cover-unreachable")
        self.assertIn("No such file", message)

    def test_a_healthy_start_is_ok_and_the_newest_line_wins(self):
        self.assertEqual(watch.classify(NOT_ANSWERING + HEALTHY_START)[0], "ok")
        self.assertEqual(watch.classify(HEALTHY_START + NOT_ANSWERING)[0], "radio-not-answering")

    def test_no_words_about_the_radio(self):
        self.assertEqual(watch.classify(["INFO  | 22:19:54 2.006 PowerFSM init, USB power=1"])[0], "unknown")

    def test_the_storm_is_counted(self):
        self.assertEqual(watch.storm(HEALTHY_START + STORM), (3, 2))
        self.assertEqual(watch.storm(HEALTHY_START), (0, 0))


class FakeRunner:
    """Answers commands the way systemd and the probe would, and remembers what was asked."""

    def __init__(self, probe_exit=1, start_exit=0):
        self.calls, self.probe_exit, self.start_exit = [], probe_exit, start_exit

    def __call__(self, command, timeout=60):
        self.calls.append(list(command))
        if command[0].endswith("lora-listen"):
            return self.probe_exit, "bridge sync after 15 reads" if self.probe_exit == 0 else "radio.begin failed: -2 (chip not found)"
        if command[:2] == ["systemctl", "start"]:
            return self.start_exit, ""
        if command[:2] == ["systemctl", "reset-failed"]:
            return 0, ""
        return 0, ""


PROBE = ["/usr/lib/meshsat/bin/lora-listen", "/dev/i2c-5", "--seconds", "1"]


class Decide(unittest.TestCase):
    def test_a_running_daemon_with_a_quiet_radio(self):
        runner = FakeRunner()
        status = watch.decide("active", HEALTHY_START, HEALTHY_START, PROBE, runner)
        self.assertEqual((status["radio"], status["action"]), ("ok", "none"))
        self.assertEqual(runner.calls, [], "nothing is probed while the daemon runs: the bus has no lock")

    def test_a_running_daemon_in_a_storm_is_noted_not_touched(self):
        runner = FakeRunner()
        status = watch.decide("active", HEALTHY_START, STORM * 4, PROBE, runner)
        self.assertEqual(status["radio"], "unstable")
        self.assertEqual(status["recoveries_10min"], 12)
        self.assertEqual(runner.calls, [])

    def test_a_failed_daemon_and_a_radio_still_dead_waits_for_a_hand(self):
        runner = FakeRunner(probe_exit=1)
        status = watch.decide("failed", HEALTHY_START + NOT_ANSWERING, [], PROBE, runner)
        self.assertEqual(status["radio"], "radio-not-answering")
        self.assertEqual(status["probe"], "no answer (exit 1)")
        self.assertEqual(status["action"], "none")
        self.assertNotIn(["systemctl", "start", "meshtasticd"], runner.calls)

    def test_a_failed_daemon_and_a_radio_that_answers_again_is_started(self):
        runner = FakeRunner(probe_exit=0)
        status = watch.decide("failed", HEALTHY_START + NOT_ANSWERING, [], PROBE, runner)
        self.assertEqual((status["radio"], status["action"]), ("recovered", "started"))
        self.assertEqual(runner.calls[0], PROBE)
        self.assertIn(["systemctl", "reset-failed", "meshtasticd"], runner.calls)
        self.assertIn(["systemctl", "start", "meshtasticd"], runner.calls)

    def test_a_cover_off_the_bus_is_not_probed(self):
        runner = FakeRunner(probe_exit=0)
        status = watch.decide("failed", UNREACHABLE_BUS, [], PROBE, runner)
        self.assertEqual(status["radio"], "cover-unreachable")
        self.assertEqual(runner.calls, [])

    def test_without_a_probe_nothing_is_started(self):
        runner = FakeRunner(probe_exit=0)
        status = watch.decide("failed", NOT_ANSWERING, [], None, runner)
        self.assertEqual(status["action"], "none")
        self.assertEqual(runner.calls, [])

    def test_the_status_file_is_written_whole(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "run", "status")
            watch.write_status(path, {"radio": "ok"})
            self.assertEqual(watch.read_status(path), {"radio": "ok"})
            self.assertEqual(watch.read_status(os.path.join(folder, "missing")), {})

    def test_the_device_comes_from_the_daemons_config(self):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(folder, "lora.yaml"), "w", encoding="utf-8") as handle:
                handle.write("Lora:\n  Module: sx1262\n  I2CDevice: /dev/i2c-2\n")
            self.assertEqual(watch.device_from_config(folder), "/dev/i2c-2")
            self.assertEqual(watch.device_from_config(os.path.join(folder, "none")), "/dev/i2c-5")


if __name__ == "__main__":
    unittest.main()
