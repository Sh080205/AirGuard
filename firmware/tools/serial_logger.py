"""Log AirGuard-S3 CSV rows from the USB serial console to files on a PC.

Pairs with the "serial CSV mirror" in firmware/main/tasks_storage.c: the
board prints every data row as ``AGCSV,<row>`` and every MARK event as
``AGEV,<row>``. This script keeps only those lines, checks them against the
CSV contract (CLAUDE.md §7, data/README.md) and writes:

  <out>/AG_YYYYMMDD_HHMMSS_usb.csv   data rows, one file per day per run
  <out>/EV_YYYYMMDD_HHMMSS_usb.csv   MARK events (same suffix as its AG file)
  <out>/serial_YYYYMMDD_HHMMSS.log   every raw line, for debugging

Default <out> is data/raw/usb/ in this repository (git-ignored like data/raw).
The *_usb suffix keeps these files apart from AG_*.csv copied off a SD card:
do not feed both copies of the same hours to ml.load.load_raw (it rejects
duplicate timestamps).

Run it from the "ESP-IDF 5.5 CMD" window (its Python has pyserial):

    python tools\\serial_logger.py --port COM7

Close idf.py monitor first: only one program can hold the COM port.
Ctrl+C stops it; files are flushed after every row.
"""

import argparse
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

ROW_PREFIX = "AGCSV,"
EVENT_PREFIX = "AGEV,"
CSV_HEADER = (
    "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,"
    "tvoc_ppb,eco2_ppm,co_ppm,label"
)
EVENT_HEADER = "start_iso,end_iso,label,note"

ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\+07:00$")
INT_RE = re.compile(r"^\d+$")
DECIMAL_RE = re.compile(r"^-?\d+\.\d$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# column index -> pattern for a non-empty value (empty = missing, allowed)
ROW_FIELDS = {1: INT_RE, 2: INT_RE, 3: INT_RE, 4: DECIMAL_RE, 5: DECIMAL_RE,
              6: INT_RE, 7: INT_RE, 8: DECIMAL_RE}
FSYNC_EVERY_ROWS = 10
STATUS_EVERY_S = 60
REOPEN_WAIT_S = 2.0


def strip_ansi(text):
    return ANSI_RE.sub("", text)


def extract(line, prefix):
    """Return the text after prefix, or None. Tolerates junk before it."""
    index = line.find(prefix)
    if index < 0:
        return None
    return line[index + len(prefix):].strip()


def parse_row(text):
    """Validate one data row; return (timestamp_text, row_text) or None."""
    fields = text.split(",")
    if len(fields) != 10 or not ISO_RE.match(fields[0]):
        return None
    if fields[9] not in ("normal", "marked"):
        return None
    for column, pattern in ROW_FIELDS.items():
        if fields[column] and not pattern.match(fields[column]):
            return None
    return fields[0], text


def parse_event(text):
    """Validate one events row start,end,marked,(note); return it or None."""
    fields = text.split(",")
    if len(fields) != 4 or not ISO_RE.match(fields[0]) or not ISO_RE.match(fields[1]):
        return None
    if fields[2] != "marked" or fields[0] >= fields[1]:
        return None
    return text


def file_stamp(iso_text):
    """2026-10-14T20:17:30+07:00 -> 20261014_201730"""
    return iso_text[0:4] + iso_text[5:7] + iso_text[8:10] + "_" + \
        iso_text[11:13] + iso_text[14:16] + iso_text[17:19]


class CsvSink:
    """Writes validated rows/events; one AG/EV file pair per day per run."""

    def __init__(self, out_dir):
        self.out_dir = Path(out_dir)
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.data = None
        self.data_day = None
        self.stamp = None
        self.events = None
        self.last_ts = None
        self.since_sync = 0
        self.rows = 0
        self.events_written = 0
        self.duplicates = 0
        self.gaps = 0
        self.gap_seconds = 0

    def _open_day(self, ts_text):
        self.close()
        self.stamp = file_stamp(ts_text)
        self.data_day = ts_text[:10]
        path = self.out_dir / f"AG_{self.stamp}_usb.csv"
        new = not path.exists() or path.stat().st_size == 0
        self.data = path.open("a", encoding="utf-8", newline="")
        if new:
            self.data.write(CSV_HEADER + "\n")
            self.data.flush()
        print(f"[logger] writing {path}", flush=True)

    def add_row(self, ts_text, row_text):
        ts = datetime.fromisoformat(ts_text)
        if self.last_ts is not None:
            step = (ts - self.last_ts).total_seconds()
            if step <= 0:
                self.duplicates += 1   # repeated or backward second: keep the first
                return
            if step > 1:
                self.gaps += 1
                self.gap_seconds += int(step) - 1
        if self.data is None or ts_text[:10] != self.data_day:
            self._open_day(ts_text)
        self.data.write(row_text + "\n")
        self.data.flush()
        self.last_ts = ts
        self.rows += 1
        self.since_sync += 1
        if self.since_sync >= FSYNC_EVERY_ROWS:
            os.fsync(self.data.fileno())
            self.since_sync = 0

    def add_event(self, event_text):
        if self.events is None:
            stamp = self.stamp or file_stamp(event_text.split(",")[0])
            path = self.out_dir / f"EV_{stamp}_usb.csv"
            new = not path.exists() or path.stat().st_size == 0
            self.events = path.open("a", encoding="utf-8", newline="")
            if new:
                self.events.write(EVENT_HEADER + "\n")
        self.events.write(event_text + "\n")
        self.events.flush()
        os.fsync(self.events.fileno())
        self.events_written += 1
        print(f"[logger] event {event_text}  -> add the event name in data/events/",
              flush=True)

    def close(self):
        for stream in (self.data, self.events):
            if stream is not None:
                stream.flush()
                os.fsync(stream.fileno())
                stream.close()
        self.data = None
        self.events = None


def open_port(port, baud):
    import serial  # pyserial: bundled with the ESP-IDF Python environment

    link = serial.Serial()
    link.port = port
    link.baudrate = baud
    link.timeout = 1
    # keep DTR/RTS released: toggling them on the USB-Serial/JTAG port can
    # reset the ESP32-S3, which would cost a 30 s PMS warm-up gap
    link.dtr = False
    link.rts = False
    link.open()
    return link


def run(args):
    import serial

    sink = CsvSink(args.out)
    raw_log = None
    if not args.no_raw_log:
        raw_path = Path(args.out) / f"serial_{datetime.now():%Y%m%d_%H%M%S}.log"
        raw_log = raw_path.open("a", encoding="utf-8", newline="")
        print(f"[logger] raw log {raw_path}", flush=True)

    bad = 0
    next_status = time.monotonic() + STATUS_EVERY_S
    link = None
    pending = b""
    try:
        while True:
            if link is None:
                try:
                    link = open_port(args.port, args.baud)
                    print(f"[logger] connected {args.port}", flush=True)
                except (serial.SerialException, OSError) as exc:
                    print(f"[logger] cannot open {args.port}: {exc}; retry in "
                          f"{REOPEN_WAIT_S:.0f} s", flush=True)
                    time.sleep(REOPEN_WAIT_S)
                    continue
            try:
                chunk = link.read(link.in_waiting or 1)
            except (serial.SerialException, OSError) as exc:
                print(f"[logger] port lost ({exc}); reopening", flush=True)
                link.close()
                link = None
                pending = b""
                continue

            pending += chunk
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = strip_ansi(raw.decode("utf-8", errors="replace")).rstrip("\r")
                if raw_log is not None:
                    raw_log.write(line + "\n")
                row = extract(line, ROW_PREFIX)
                event = extract(line, EVENT_PREFIX) if row is None else None
                if row is not None:
                    parsed = parse_row(row)
                    if parsed is None:
                        bad += 1
                        print(f"[logger] BAD row ignored: {line!r}", flush=True)
                    else:
                        sink.add_row(*parsed)
                elif event is not None:
                    parsed = parse_event(event)
                    if parsed is None:
                        bad += 1
                        print(f"[logger] BAD event ignored: {line!r}", flush=True)
                    else:
                        sink.add_event(parsed)
                elif not args.quiet and line:
                    print(line, flush=True)
            if raw_log is not None and lines:
                raw_log.flush()

            if time.monotonic() >= next_status:
                next_status += STATUS_EVERY_S
                print(f"[logger] rows={sink.rows} events={sink.events_written} "
                      f"bad={bad} duplicate={sink.duplicates} gaps={sink.gaps} "
                      f"({sink.gap_seconds} s missing) last={sink.last_ts}",
                      flush=True)
    except KeyboardInterrupt:
        print("\n[logger] stopped", flush=True)
    finally:
        if link is not None:
            link.close()
        sink.close()
        if raw_log is not None:
            raw_log.close()
        print(f"[logger] total rows={sink.rows} events={sink.events_written} "
              f"bad={bad} duplicate={sink.duplicates} gaps={sink.gaps} "
              f"({sink.gap_seconds} s missing)", flush=True)
    return 0


def main(argv=None):
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True, help="e.g. COM7")
    parser.add_argument("--baud", type=int, default=115200,
                        help="ignored by USB-Serial/JTAG; used on a UART bridge")
    parser.add_argument("--out", default=str(repo / "data" / "raw" / "usb"),
                        help="output folder (default: data/raw/usb in the repo)")
    parser.add_argument("--quiet", action="store_true",
                        help="do not echo the board's log lines")
    parser.add_argument("--no-raw-log", action="store_true",
                        help="do not keep serial_*.log")
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
