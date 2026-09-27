#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the verdict must never do: call a frame received without a complete line that says so,
or blame the radio for a capture that was not running."""
import json
import os
import tempfile
import unittest

import verdict

SENDER = "0x4d530a4a"
OTHER = "0x27ca8f1c"


def ok(uptime, packet_id, length, sender=SENDER, offset=4300.5):
    return [
        f"DEBUG | 16:00:00 {uptime} [RadioIf] Corrected frequency offset: {offset}",
        f"DEBUG | 16:00:00 {uptime} [RadioIf] Lora RX (id={packet_id} fr={sender} to=0xffffffff, transport = 0, "
        f"WantAck=0, HopLim=0 Ch=0x5a encrypted len={length} rxSNR=6.5 rxRSSI=-58)",
        f"DEBUG | 16:00:00 {uptime} [RadioIf] Packet RX: 477ms",
    ]


def refused(uptime, packet_id, error=-7, sender=SENDER):
    return [
        f"ERROR | 16:00:00 {uptime} [RadioIf] Ignore received packet due to error={error} (maybe id={packet_id} "
        f"fr={sender} to=0xffffffff flags=0x00 rxSNR=5.25 rxRSSI=-59 nextHop=0x0 relay=0x4a)",
        f"DEBUG | 16:00:00 {uptime} [RadioIf] Packet RX (noise?) : 805ms",
    ]


def alive(first, last, step=20):
    """The lines a quiet receiver writes anyway."""
    return [f"DEBUG | 16:00:00 {u} [Power] Battery: usbPower=1, isCharging=1, batMv=4197, batPct=100" for u in range(first, last, step)]


def captured(lines, wall, beats=()):
    """The same lines as capture.py keeps them, read at `wall`, with the capture's own signs."""
    out = [json.dumps({"kind": "line", "wall": wall, "mono": wall - 1e9, "text": text}) for text in lines]
    return out + [json.dumps({"kind": "heartbeat", "wall": beat, "mono": beat - 1e9}) for beat in beats]


def attempt(name, packet_id, length, t_end, airtime_ms=1000, sender=SENDER):
    return {"attempt": name, "id": packet_id, "sender": sender, "length": length, "t_end": t_end, "airtime_ms": airtime_ms}


class Verdicts(unittest.TestCase):
    def run_case(self, lines, attempts, **options):
        if not any(line.startswith("{") for line in lines):
            lines = sorted(lines, key=lambda l: int(verdict.LINE.search(l).group(3)) if verdict.LINE.search(l) else 0)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "receiver.log")
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as handle:
                handle.write("\n".join(lines) + "\n")
            rows = verdict.judge(attempts, verdict.read_log(path, "receiver.log"), **options)
        self.assertEqual(len(rows), len(attempts), "every attempt gets exactly one row")
        return {row["attempt"]: row for row in rows}

    # The sender's clock runs 50000 s ahead of the receiver's uptime in these cases.
    def test_a_complete_line_is_needed_to_accept(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + refused(1040, "0x22222222"),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["a"]["verdict"], "accepted")
        self.assertEqual(rows["a"]["offset_hz"], 4300.5)
        self.assertEqual(rows["a"]["content"], "not_checked")
        self.assertEqual(rows["b"]["verdict"], "crc_failed")
        self.assertEqual(rows["b"]["error_codes"], [-7])
        # The receiver's airtime for the length it read: the only trace of a refused frame's length.
        self.assertEqual(rows["a"]["rx_time_ms"], 477)
        self.assertEqual(rows["b"]["rx_time_ms"], 805)

    def test_another_error_is_not_a_reception(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + refused(1040, "0x22222222", error=-706),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "radio_error")
        self.assertEqual(rows["b"]["error_codes"], [-706])

    def test_a_line_that_only_mentions_the_id_is_not_a_reception(self):
        mention = ["DEBUG | 16:00:00 1040 [Router] Forwarding to phone (id=0x22222222 fr=0x4d530a4a to=0xffffffff, len=76)"]
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + mention,
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "not_observed")

    def test_a_cut_line_gives_no_verdict(self):
        cut = ["DEBUG | 16:00:00 1040 [RadioIf] Lora RX (id=0x22222222 fr=0x4d530a4a to=0xffffffff, transport = 0, Wan"]
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + cut,
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_a_line_cut_inside_the_id_gives_no_verdict_either(self):
        cut = ["DEBUG | 16:00:00 1040 [RadioIf] Lora RX (id=0x2222"]
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + cut,
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_an_id_used_twice_gives_no_verdict(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + ok(1040, "0x11111111", 32),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x11111111", 32, 51040)],
        )
        self.assertEqual({rows["a"]["verdict"], rows["b"]["verdict"]}, {"ambiguous"})

    def test_accepted_and_refused_for_one_id_gives_no_verdict(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + ok(1040, "0x22222222", 76) + refused(1041, "0x22222222"),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_a_capture_that_stopped_is_not_held_against_the_radio(self):
        rows = self.run_case(
            alive(900, 1020) + ok(1000, "0x11111111", 32) + alive(1400, 1500),
            [
                attempt("a", "0x11111111", 32, 51000),
                attempt("in the gap", "0x22222222", 76, 51200),
                attempt("after the end", "0x33333333", 76, 51700),
            ],
        )
        self.assertEqual(rows["in the gap"]["verdict"], "capture_invalid")
        self.assertEqual(rows["after the end"]["verdict"], "capture_invalid")

    def test_no_capture_at_all_gives_no_verdict(self):
        rows = self.run_case([], [attempt("a", "0x11111111", 32, 51000)])
        self.assertEqual(rows["a"]["verdict"], "capture_invalid")

    def test_a_wrong_length_is_not_a_reception(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + ok(1040, "0x22222222", 60),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_a_receiver_that_was_sending_cannot_testify(self):
        sending = [
            "DEBUG | 16:00:00 1039 [RadioIf] Started Tx (id=0x0badcafe fr=0xa1b3c2ec to=0xffffffff, transport = 0)",
            "DEBUG | 16:00:00 1041 [RadioIf] Completed sending (id=0x0badcafe fr=0xa1b3c2ec to=0xffffffff, transport = 0)",
        ]
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + sending,
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_a_refused_packet_with_another_id_in_the_frames_time(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + refused(1040, "0x2222ffff"),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_the_right_id_at_the_wrong_time_is_not_a_reception(self):
        rows = self.run_case(
            alive(900, 1600)
            + ok(1000, "0x11111111", 32)
            + ok(1010, "0x33333333", 32)
            + ok(1020, "0x44444444", 32)
            + ok(1500, "0x22222222", 76),
            [
                attempt("a", "0x11111111", 32, 51000),
                attempt("c", "0x33333333", 32, 51010),
                attempt("d", "0x44444444", 32, 51020),
                attempt("b", "0x22222222", 76, 51040),
            ],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_a_packet_of_another_sender_with_our_id(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + ok(1040, "0x22222222", 76, sender=OTHER),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "ambiguous")

    def test_colour_codes_and_client_bytes_before_a_line(self):
        lines = alive(900, 1200) + ok(1000, "0x11111111", 32)
        lines[-2] = "\x94\xc3\x00\x1dSM�� E  \x1b[34m" + lines[-2].replace("DEBUG", "DEBUG ") + "\x1b[0m\r"
        rows = self.run_case(lines, [attempt("a", "0x11111111", 32, 51000)])
        self.assertEqual(rows["a"]["verdict"], "accepted")

    def test_the_daemons_wording_of_a_refusal(self):
        daemon = [
            "ERROR | 21:48:54 1040 [RadioIf] Ignore rx packet, error=-7 (maybe id=0x22222222 fr=0x4d530a4a "
            "to=0xffffffff flags=0xfa rxSNR=25.5 rxRSSI=-33 nextHop=0x2d relay=0x51)"
        ]
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + daemon,
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)],
        )
        self.assertEqual(rows["b"]["verdict"], "crc_failed")

    def test_a_capture_file_is_read_by_the_time_each_line_was_read(self):
        # The receiver's own clock and uptime say nothing here: the sender's clock and the capture's agree.
        beats = range(50900, 51200, 5)
        lines = (
            captured(alive(1, 2), 50990)
            + captured(ok(7, "0x11111111", 32), 51000)
            + captured(refused(9, "0x22222222"), 51040)
            + captured(alive(3, 4), 51090)
            + captured([], 0, beats)
        )
        rows = self.run_case(lines, [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040)])
        self.assertEqual(rows["a"]["verdict"], "accepted")
        self.assertEqual(rows["a"]["offset_hz"], 4300.5)
        self.assertEqual(rows["b"]["verdict"], "crc_failed")

    def test_a_silent_receiver_is_told_from_a_capture_that_was_not_running(self):
        lines = (
            captured(alive(1, 2), 50990)
            + captured(ok(7, "0x11111111", 32), 51000)
            + captured(alive(3, 4), 51020)
            + captured([], 0, range(50900, 51400, 5))
            + captured(alive(5, 6), 51900)
        )
        rows = self.run_case(
            lines,
            [
                attempt("a", "0x11111111", 32, 51000),
                attempt("receiver silent", "0x22222222", 76, 51200),
                attempt("capture gone", "0x33333333", 76, 51700),
            ],
        )
        self.assertEqual(rows["a"]["verdict"], "accepted")
        self.assertEqual(rows["receiver silent"]["verdict"], "capture_invalid")
        self.assertEqual(rows["receiver silent"]["capture"], "receiver_silent")
        self.assertEqual(rows["capture gone"]["verdict"], "capture_invalid")
        self.assertEqual(rows["capture gone"]["capture"], "does_not_cover")

    def test_the_payload_is_the_one_the_tool_sends(self):
        # The same bytes as test/test_tools.cpp expects of tools/lora-ping/payload.h.
        self.assertEqual(verdict.expected_payload("0x12345678", 8), bytes([0x87, 0x15, 0x48, 0x81, 0x70, 0x29, 0x89, 0xC5]))
        self.assertEqual(verdict.expected_payload(0, 4), bytes([0xA2, 0x97, 0xCD, 0xAE]))

    def test_the_tools_own_file_gives_the_frames(self):
        lines = [
            {"t": 51000.0, "event": "attempt", "tool": "lora-ping", "run": "0badcafe", "frame": 1, "id": "0x11111111",
             "sender": SENDER, "length": 32, "airtime_ms": 477, "from": "standby"},
            {"t": 51001.4, "event": "outcome", "run": "0badcafe", "frame": 1, "id": "0x11111111", "tx_done": True,
             "mode_before": 2, "mode_loaded": 2},
            {"t": 51040.0, "event": "attempt", "tool": "lora-ping", "run": "0badcafe", "frame": 2, "id": "0x22222222",
             "sender": SENDER, "length": 192, "airtime_ms": 2845, "from": "standby"},
            {"t": 51090.0, "event": "attempt", "tool": "meshtasticd", "id": "0x33333333", "sender": "0x52cb81e7",
             "length": 176, "airtime_ms": 2722},
        ]
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "airtime.jsonl")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("\n".join(json.dumps(line) for line in lines) + "\n")
            frames = verdict.load_attempts(path)
        self.assertEqual([f["attempt"] for f in frames], ["0badcafe-1", "0badcafe-2", "daemon-0x33333333-51090"])
        self.assertEqual(frames[0]["t_end"], 51001.4)
        # A run that was cut off has no outcome for its last frame: the frame is judged all the same.
        self.assertAlmostEqual(frames[1]["t_end"], 51040.0 + 4.345, places=3)
        self.assertEqual(frames[2]["t_end"], 51090.0)
        self.assertEqual(frames[0]["outcome"]["mode_loaded"], 2)

    def test_the_counts_add_up(self):
        rows = self.run_case(
            alive(900, 1200) + ok(1000, "0x11111111", 32) + refused(1040, "0x22222222"),
            [attempt("a", "0x11111111", 32, 51000), attempt("b", "0x22222222", 76, 51040), attempt("c", "0x33333333", 76, 51080)],
        )
        counts = verdict.summary(list(rows.values()))
        self.assertEqual(counts["sent"], 3)
        self.assertEqual(sum(v for k, v in counts.items() if k != "sent"), 3)


if __name__ == "__main__":
    unittest.main()
