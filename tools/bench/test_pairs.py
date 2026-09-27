#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The paired reading, against figures worked out by hand."""
import unittest

import pairs


def row(block, arm, verdict, repeat=False, void=False):
    return {"label": f"run-b{block}{'r' if repeat else ''}-{arm}", "verdict": verdict, "void": void}


class Pairs(unittest.TestCase):
    def test_the_exact_probability_of_the_disagreeing_blocks(self):
        # the figures the reviewer gave for 6 of 8 against 2 of 8, by which blocks disagree
        self.assertAlmostEqual(pairs.one_sided_p(6, 2), 0.1445, places=4)
        self.assertAlmostEqual(pairs.one_sided_p(5, 1), 0.1094, places=4)
        self.assertAlmostEqual(pairs.one_sided_p(4, 0), 0.0625, places=4)
        self.assertEqual(pairs.one_sided_p(0, 0), 1.0)
        self.assertAlmostEqual(pairs.one_sided_p(8, 0), 1 / 256, places=6)

    def test_agreeing_blocks_say_nothing_about_the_difference(self):
        rows = []
        for b in range(1, 9):
            rows.append(row(b, "B", "accepted" if b <= 6 else "crc_failed"))
            rows.append(row(b, "C", "accepted" if b <= 2 else "crc_failed"))
        summary = pairs.summarise(pairs.read_pairs(rows, "B", "C"), "B", "C")
        self.assertEqual(summary["accepted"], {"B": 6, "C": 2})
        self.assertEqual(summary["both_accepted"], 2)
        self.assertEqual(summary["neither_accepted"], 2)
        self.assertEqual(summary["only_B_accepted"], 4)
        self.assertEqual(summary["only_C_accepted"], 0)
        self.assertAlmostEqual(summary["one_sided_p_if_arms_equal"], 0.0625, places=4)

    def test_a_void_frame_makes_its_pair_incomplete_and_a_repeat_stands_in(self):
        rows = [row(1, "B", "accepted", void=True), row(1, "C", "crc_failed"), row(2, "B", "accepted"), row(2, "C", "crc_failed")]
        summary = pairs.summarise(pairs.read_pairs(rows, "B", "C"), "B", "C")
        self.assertEqual(summary["incomplete_pairs"], 1)
        self.assertEqual(summary["complete_pairs"], 1)
        rows += [row(1, "B", "crc_failed", repeat=True), row(1, "C", "crc_failed", repeat=True)]
        summary = pairs.summarise(pairs.read_pairs(rows, "B", "C"), "B", "C")
        self.assertEqual(summary["incomplete_pairs"], 0)
        self.assertEqual(summary["neither_accepted"], 1)
        self.assertEqual(summary["only_B_accepted"], 1)

    def test_a_repeat_does_not_replace_a_frame_that_stood(self):
        rows = [row(1, "B", "crc_failed"), row(1, "C", "crc_failed"), row(1, "B", "accepted", repeat=True), row(1, "C", "accepted", repeat=True)]
        blocks = pairs.read_pairs(rows, "B", "C")
        self.assertEqual(blocks[1]["B"]["verdict"], "crc_failed")
        self.assertFalse(blocks[1]["B"]["repeat"])

    def test_frames_without_a_verdict_leave_the_pair_incomplete(self):
        rows = [row(1, "B", "accepted"), row(1, "C", "capture_invalid"), row(2, "B", "ambiguous"), row(2, "C", "crc_failed")]
        summary = pairs.summarise(pairs.read_pairs(rows, "B", "C"), "B", "C")
        self.assertEqual(summary["incomplete_pairs"], 2)
        self.assertEqual(summary["judged"], {"B": 1, "C": 1})


if __name__ == "__main__":
    unittest.main()
