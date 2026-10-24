"""Host tests for ze07_sim.py (stdlib unittest, no board, no pyserial).

    python -m unittest discover -s firmware/tools -p "test_*.py" -v
"""

import unittest

import ze07_sim as zs

# Winsen ZE07-CO manual V1.7 p.4 Table 4, typed by hand (3.7 ppm).
TABLE4 = bytes([0xFF, 0x04, 0x03, 0x01, 0x00, 0x25, 0x13, 0x88, 0x38])


class FrameTests(unittest.TestCase):
    def test_table4_example(self):
        self.assertEqual(zs.make_frame(37), TABLE4)

    def test_checksum_matches_manual_command_examples(self):
        # p.5 Tables 5-8: same checksum rule for commands and answers
        for frame in ([0xFF, 0x01, 0x78, 0x41, 0, 0, 0, 0, 0x46],
                      [0xFF, 0x01, 0x78, 0x40, 0, 0, 0, 0, 0x47],
                      [0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79],
                      [0xFF, 0x86, 0x00, 0x20, 0, 0, 0x00, 0x20, 0x3A]):
            self.assertEqual(zs.checksum(frame), frame[8])

    def test_values_and_bad_frame(self):
        self.assertEqual(zs.make_frame(950)[4:6], bytes([0x03, 0xB6]))
        self.assertEqual(zs.make_frame(0xFFFF)[4:6], b"\xff\xff")
        bad = zs.make_frame(37, good=False)
        self.assertNotEqual(zs.checksum(bad), bad[8])
        self.assertEqual(bad[:8], TABLE4[:8])
        with self.assertRaises(ValueError):
            zs.make_frame(-1)
        with self.assertRaises(ValueError):
            zs.make_frame(0x10000)


class ScriptTests(unittest.TestCase):
    def test_ppm_text_is_exact(self):
        self.assertEqual(zs.ppm_to_x10("3.7"), 37)
        self.assertEqual(zs.ppm_to_x10("90"), 900)
        self.assertEqual(zs.ppm_to_x10("89.9"), 899)
        self.assertEqual(zs.ppm_to_x10("1."), 10)
        for bad in ("-1", "1.25", "abc", ""):
            with self.assertRaises(ValueError):
                zs.ppm_to_x10(bad)

    def test_parse_script(self):
        self.assertEqual(zs.parse_script("3.7:20, off:10,BAD:2,95:0.5"), [
            ("ppm", 37, 20.0), ("off", None, 10.0), ("bad", None, 2.0), ("ppm", 950, 0.5)])
        for bad in ("3.7", "3.7:0", "x:1", "3.7:-1"):
            with self.assertRaises(ValueError):
                zs.parse_script(bad)


class FakeLink:
    def __init__(self):
        self.frames = []

    def write(self, data):
        self.frames.append(bytes(data))


class RunTests(unittest.TestCase):
    def test_timing_and_segments(self):
        now = [0.0]
        link = FakeLink()
        sent = zs.run(link, zs.parse_script("3.7:3,off:2,bad:1,95:2"), 1.0,
                      clock=lambda: now[0],
                      sleep=lambda s: now.__setitem__(0, now[0] + s),
                      log=lambda *_: None)
        self.assertEqual(sent, 6)                 # 3 + 0 + 1 + 2
        self.assertEqual(link.frames[:3], [TABLE4] * 3)
        bad = link.frames[3]
        self.assertNotEqual(zs.checksum(bad), bad[8])
        self.assertEqual(link.frames[4], zs.make_frame(950))
        self.assertAlmostEqual(now[0], 7.0)       # last frame sent at t = 7 s


if __name__ == "__main__":
    unittest.main()
