"""Host tests for serial_logger.py (stdlib unittest, no board, no pyserial).

    python -m unittest discover -s firmware/tools -p "test_*.py" -v
"""

import csv
import tempfile
import unittest
from datetime import datetime, timedelta
from pathlib import Path

import serial_logger as sl

ROW = "2026-10-14T20:17:30+07:00,8,12,15,28.4,62.1,,,,normal"


def row_at(second, label="normal"):
    ts = datetime(2026, 10, 14, 20, 17, 30) + timedelta(seconds=second)
    return f"{ts:%Y-%m-%dT%H:%M:%S}+07:00,8,12,15,28.4,62.1,,,,{label}"


class ParseTests(unittest.TestCase):
    def test_header_matches_contract(self):
        # CLAUDE.md §7 / ml/load.py RAW_COLUMNS
        self.assertEqual(sl.CSV_HEADER.split(","), [
            "timestamp_iso", "pm1_0", "pm2_5", "pm10", "temp_c", "humidity_pct",
            "tvoc_ppb", "eco2_ppm", "co_ppm", "label"])
        self.assertEqual(sl.EVENT_HEADER, "start_iso,end_iso,label,note")

    def test_extract_strips_prefix_and_junk(self):
        self.assertEqual(sl.extract("AGCSV," + ROW, sl.ROW_PREFIX), ROW)
        self.assertEqual(sl.extract("xx AGCSV," + ROW + "\r", sl.ROW_PREFIX), ROW)
        self.assertIsNone(sl.extract("I (61082) sampler: last 60s", sl.ROW_PREFIX))

    def test_agml_line_is_neither_row_nor_event(self):
        # FW-7 step 3: the per-row ML score line printed after each AGCSV row
        # must stay out of the CSV files (CLAUDE.md §7); run() then only
        # keeps it in serial_*.log (and echoes it unless --quiet).
        for line in ("AGML,2026-10-24T09:05:07+07:00,35.2500,above,2",
                     "AGML,2026-10-24T09:05:07+07:00,,missing,0",
                     "AGML,2026-10-24T09:05:07+07:00,,model_fault,0"):
            with self.subTest(line=line):
                self.assertIsNone(sl.extract(line, sl.ROW_PREFIX))
                self.assertIsNone(sl.extract(line, sl.EVENT_PREFIX))

    def test_strip_ansi(self):
        self.assertEqual(sl.strip_ansi("\x1b[0;32mI (1) x: y\x1b[0m"), "I (1) x: y")

    def test_valid_rows(self):
        self.assertEqual(sl.parse_row(ROW), (ROW[:25], ROW))
        missing = "2026-10-14T20:17:30+07:00,,,,,,,,,marked"   # all empty = allowed
        self.assertIsNotNone(sl.parse_row(missing))
        full = "2026-10-14T20:17:30+07:00,8,12,15,-0.4,62.1,145,520,0.4,normal"
        self.assertIsNotNone(sl.parse_row(full))

    def test_invalid_rows(self):
        bad = [
            ROW.replace("+07:00", "Z"),                 # timezone
            ROW.replace(",normal", ",incense"),         # label
            ROW + ",extra",                             # 11 columns
            ROW.rsplit(",", 1)[0],                      # 9 columns
            ROW.replace(",8,", ",8.0,"),                # pm must be integer
            ROW.replace(",28.4,", ",28.45,"),           # one decimal only
            ROW.replace(",8,", ",-1,"),                 # no -1 for missing
            "2026-10-14T20:17I (5) storage: SD mount failed",  # split by a log line
        ]
        for text in bad:
            with self.subTest(text=text):
                self.assertIsNone(sl.parse_row(text))

    def test_events(self):
        ok = "2026-10-13T21:46:23+07:00,2026-10-13T21:46:27+07:00,marked,"
        self.assertEqual(sl.parse_event(ok), ok)
        self.assertIsNone(sl.parse_event(ok.replace("marked", "normal")))
        swapped = "2026-10-13T21:46:27+07:00,2026-10-13T21:46:23+07:00,marked,"
        self.assertIsNone(sl.parse_event(swapped))   # ml.load requires start < end

    def test_file_stamp(self):
        self.assertEqual(sl.file_stamp(ROW[:25]), "20261014_201730")


class SinkTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, name):
        with (self.out / name).open(encoding="utf-8", newline="") as stream:
            return list(csv.reader(stream))

    def test_ten_minutes_one_file(self):
        sink = sl.CsvSink(self.out)
        for second in range(600):
            sink.add_row(*sl.parse_row(row_at(second)))
        sink.close()
        rows = self.read("AG_20261014_201730_usb.csv")
        self.assertEqual(rows[0], sl.CSV_HEADER.split(","))
        self.assertEqual(len(rows) - 1, 600)
        self.assertEqual((sink.rows, sink.gaps, sink.duplicates), (600, 0, 0))

    def test_duplicate_and_gap_counted(self):
        sink = sl.CsvSink(self.out)
        for second in (0, 1, 1, 0, 5):
            sink.add_row(*sl.parse_row(row_at(second)))
        sink.close()
        self.assertEqual(sink.rows, 3)          # 0, 1, 5
        self.assertEqual(sink.duplicates, 2)    # repeated 1, backward 0
        self.assertEqual((sink.gaps, sink.gap_seconds), (1, 3))

    def test_midnight_starts_new_file(self):
        sink = sl.CsvSink(self.out)
        sink.add_row(*sl.parse_row("2026-10-14T23:59:59+07:00,,,,,,,,,normal"))
        sink.add_row(*sl.parse_row("2026-10-15T00:00:00+07:00,,,,,,,,,normal"))
        sink.close()
        self.assertEqual(len(self.read("AG_20261014_235959_usb.csv")), 2)
        self.assertEqual(len(self.read("AG_20261015_000000_usb.csv")), 2)

    def test_event_file_pairs_with_data_file(self):
        sink = sl.CsvSink(self.out)
        sink.add_row(*sl.parse_row(row_at(0)))
        sink.add_event("2026-10-14T20:17:31+07:00,2026-10-14T20:17:40+07:00,marked,")
        sink.close()
        rows = self.read("EV_20261014_201730_usb.csv")
        self.assertEqual(rows[0], sl.EVENT_HEADER.split(","))
        self.assertEqual(rows[1][2], "marked")

    def test_output_loads_with_ml_loader_rules(self):
        # same checks ml/load.py applies: header, 10 columns, unique +07:00 times
        sink = sl.CsvSink(self.out)
        for second in range(5):
            sink.add_row(*sl.parse_row(row_at(second, "marked" if second == 2 else "normal")))
        sink.close()
        rows = self.read("AG_20261014_201730_usb.csv")
        self.assertTrue(all(len(r) == 10 for r in rows))
        stamps = [r[0] for r in rows[1:]]
        self.assertEqual(len(stamps), len(set(stamps)))
        self.assertTrue(all(s.endswith("+07:00") for s in stamps))


if __name__ == "__main__":
    unittest.main()
