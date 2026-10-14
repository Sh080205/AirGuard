"""Host tests for heater_compare.py (stdlib unittest, no board).

    python -m unittest discover -s firmware/tools -p "test_*.py" -v
"""

import contextlib
import io
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

import heater_compare as hc

TZ = timezone(timedelta(hours=7))
T0 = datetime(2026, 10, 16, 19, 0, 0, tzinfo=TZ)
HEADER = "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,tvoc_ppb,eco2_ppm,co_ppm,label"


def iso(t):
    return t.isoformat()


def row(t, temp, rh):
    tt = "" if temp is None else f"{temp:.1f}"
    rr = "" if rh is None else f"{rh:.1f}"
    return f"{iso(t)},8,12,15,{tt},{rr},,,,normal"


class HelperTests(unittest.TestCase):
    def test_describe(self):
        d = hc.describe([1.0, 2.0, 3.0])
        self.assertEqual((d["n"], d["mean"], d["min"], d["max"]), (3, 2.0, 1.0, 3.0))
        self.assertAlmostEqual(d["std"], 1.0)          # sample std
        self.assertEqual(hc.describe([5.0])["std"], 0.0)
        self.assertIsNone(hc.describe([])["mean"])

    def test_slope_per_hour(self):
        pts = [(T0 + timedelta(minutes=m), 25.0 + 0.5 * m / 60) for m in range(0, 61, 5)]
        self.assertAlmostEqual(hc.slope_per_hour(pts), 0.5)
        self.assertIsNone(hc.slope_per_hour(pts[:1]))
        self.assertIsNone(hc.slope_per_hour([(T0, 1.0), (T0, 2.0)]))

    def test_phase_rows_settle_and_inclusive_end(self):
        rows = [(T0 + timedelta(minutes=m), 1.0, 2.0) for m in range(0, 31)]
        sel = hc.phase_rows(rows, T0, T0 + timedelta(minutes=30), timedelta(minutes=10))
        self.assertEqual([r[0].minute for r in sel], list(range(10, 31)))

    def test_parse_time_needs_offset(self):
        self.assertEqual(hc.parse_time("2026-10-16T19:00:00+07:00"), T0)
        with self.assertRaises(ValueError):
            hc.parse_time("2026-10-16T19:00:00")

    def test_parse_compare(self):
        self.assertEqual(hc.parse_compare("on=off,off2", ["off", "on", "off2"]),
                         ("on", ["off", "off2"]))
        for bad in ("on", "on=", "x=off", "on=nope"):
            with self.assertRaises(ValueError):
                hc.parse_compare(bad, ["off", "on", "off2"])

    def test_nearest_row_window(self):
        rows = [(T0 + timedelta(seconds=s), float(s), None) for s in (0, 40, 100)]
        self.assertEqual(hc.nearest_row(rows, T0 + timedelta(seconds=35),
                                        timedelta(seconds=30))[1], 40.0)
        self.assertIsNone(hc.nearest_row(rows, T0 + timedelta(seconds=70),
                                         timedelta(seconds=20)))


class EndToEndTests(unittest.TestCase):
    """off / on / off with a linear room drift of +0.6 C/h and a heater
    step of +0.4 C, -1.5 %RH while on: ABA must recover the step."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        lines = [HEADER]
        refs = ["time_iso,temp_c,humidity_pct"]
        for s in range(0, 90 * 60, 1):
            t = T0 + timedelta(seconds=s)
            room = 25.0 + 0.6 * s / 3600
            heater_on = 30 * 60 <= s < 60 * 60
            temp = room + (0.4 if heater_on else 0.0)
            rh = 60.0 - (1.5 if heater_on else 0.0)
            lines.append(row(t, None if s % 97 == 0 else temp, rh))   # some gaps
            if s % 300 == 0:
                refs.append(f"{iso(t)},{room:.2f},60.0")
        self.csv = self.dir / "AG_usb.csv"
        self.csv.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.ref = self.dir / "ref.csv"
        self.ref.write_text("\n".join(refs) + "\n", encoding="utf-8")
        self.phases = []
        for name, a, b in (("off", 0, 30), ("on", 30, 60), ("off2", 60, 90)):
            self.phases += ["--phase", name, iso(T0 + timedelta(minutes=a)),
                            iso(T0 + timedelta(minutes=b) - timedelta(seconds=1))]

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = hc.main(["--csv", str(self.csv)] + self.phases + extra)
        return code, out.getvalue()

    def test_aba_cancels_linear_drift(self):
        code, text = self.run_main(["--compare", "on=off,off2"])
        self.assertEqual(code, 0, text)
        line = [l for l in text.splitlines() if l.strip().startswith("on - mean(off,off2)")][0]
        # one-decimal CSV values round the drift; 0.05 C / 0.05 %RH tolerance
        dt = float(line.split("dT = ")[1].split()[0])
        drh = float(line.split("dRH = ")[1].split()[0])
        self.assertAlmostEqual(dt, 0.4, delta=0.05)
        self.assertAlmostEqual(drh, -1.5, delta=0.05)

    def test_ab_only_is_biased_by_drift(self):
        code, text = self.run_main(["--compare", "on=off"])
        self.assertEqual(code, 0, text)
        line = [l for l in text.splitlines() if "on - mean(off)" in l][0]
        dt = float(line.split("dT = ")[1].split()[0])
        self.assertAlmostEqual(dt, 0.4 + 0.3, delta=0.05)   # 30 min x 0.6 C/h

    def test_reference_offset_removes_drift(self):
        code, text = self.run_main(["--compare", "on=off", "--ref", str(self.ref)])
        self.assertEqual(code, 0, text)
        line = [l for l in text.splitlines() if "on - mean(off)" in l][0]
        ref_part = line.split("vs reference:")[1]
        self.assertAlmostEqual(float(ref_part.split("dT = ")[1].split()[0]), 0.4, delta=0.06)
        self.assertAlmostEqual(float(ref_part.split("dRH = ")[1].split()[0]), -1.5, delta=0.06)

    def test_default_compares_against_first_phase(self):
        code, text = self.run_main([])
        self.assertEqual(code, 0, text)
        self.assertIn("on - mean(off)", text)
        self.assertIn("off2 - mean(off)", text)

    def test_bad_input_returns_2(self):
        code, text = self.run_main(["--compare", "on=missing"])
        self.assertEqual(code, 2)
        self.assertIn("unknown phase", text)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = hc.main(["--csv", str(self.csv), "--phase", "x", iso(T0),
                            iso(T0 + timedelta(minutes=5))])
        self.assertEqual(code, 2)   # shorter than the 10 min settle


if __name__ == "__main__":
    unittest.main()
