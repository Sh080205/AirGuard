"""R2: ซ้อมรันทั้งสายด้วยข้อมูลสังเคราะห์ที่จัดรูปแบบเหมือนไฟล์จาก serial_logger.

ข้อมูลทั้งหมดในโมดูลนี้ = สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง · ผลโมเดลจากการซ้อม
ใช้ยืนยันว่าคำสั่งรันจบเท่านั้น ห้ามรายงานเป็นผลโมเดล

    python -m ml.dryrun make --days 5 --seed 42 --out data/processed/dryrun
        → <out>/usb/AG_*_usb.csv · EV_*_usb.csv (แบบ serial_logger) + <out>/truth_events.csv
    python -m ml.dryrun fill --draft <ร่างจาก ml.events draft> --truth <out>/truth_events.csv --out <events>
        → แทนการแก้ TODO ด้วยมือ (วันจริงคนแก้เองจากใบจด)
    python -m ml.dryrun check-header <ag_mahal_pm_th.h>
        → คอมไพล์ header ด้วย gcc แล้วเทียบ golden (harness เดียวกับ test ของ ML-3)
"""

import argparse
import csv
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd

try:
    from .load import (
        EVENT_COLUMNS,
        INTEGER_COLUMNS,
        NUMERIC_COLUMNS,
        RAW_COLUMNS,
        load_events,
    )
    from .synth import generate
except ImportError:  # `python ml/dryrun.py`
    from load import (
        EVENT_COLUMNS,
        INTEGER_COLUMNS,
        NUMERIC_COLUMNS,
        RAW_COLUMNS,
        load_events,
    )
    from synth import generate

# รีบูตจำลอง: (วันที่, ชั่วโมง, วินาทีที่ไม่มีแถว = บูต + รอ SNTP) — สมมติฐาน
# ครั้งที่สอง SNTP นานกว่า 30 s → PM มีค่าทันที ต้องจับจาก ENS160/CO (ml/qa.py)
REBOOTS = ((1, 3, 20), (2, 3, 45))
# ช่วงที่เฟิร์มแวร์เว้นว่างนับจากบูต: PMS_WARMUP_US 30 s · ENS160 warm-up วัดได้ 190 s (21 ต.ค.)
# · CO_WARMUP_US 300 s
BOOT_BLANK_S = {
    "pm1_0": 30,
    "pm2_5": 30,
    "pm10": 30,
    "tvoc_ppb": 190,
    "eco2_ppm": 190,
    "co_ppm": 300,
}
# แถวเดี่ยวที่หายระหว่าง USB (วันที่, ชั่วโมง, นาที) — สมมติฐาน
TRANSIT_DROPS = ((0, 5, 0), (1, 9, 30), (2, 16, 45))
NOTE = "dry run: สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง"


def _stamp(time):
    return time.strftime("%Y%m%d_%H%M%S")


def _write_raw(frame, path):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(RAW_COLUMNS)
        for row in frame.itertuples(index=False, name=None):
            cells = [row[0]]
            for column, value in zip(NUMERIC_COLUMNS, row[1:-1], strict=True):
                spec = ".0f" if column in INTEGER_COLUMNS else ".1f"
                cells.append("" if pd.isna(value) else format(value, spec))
            writer.writerow([*cells, row[-1]])


def make(out, days=5, seed=42):
    """เขียนไฟล์แบบ serial_logger: แยกไฟล์รายวัน · รีบูต 2 ครั้ง · แถวหาย 3 แถว · EV ต่อวัน"""
    if days < 3:
        raise ValueError("days must be >= 3 (reboots on day 1 and 2)")
    raw, events = generate(days=days, seed=seed)
    times = pd.DatetimeIndex(pd.to_datetime(raw["timestamp_iso"]))
    start = times[0]
    ev_start = pd.to_datetime(events["start_iso"])
    ev_end = pd.to_datetime(events["end_iso"])

    def row_at(day, hour, minute=0):
        return int((pd.Timedelta(days=day, hours=hour, minutes=minute)).total_seconds())

    def in_event(time):
        return bool(((ev_start <= time) & (time < ev_end)).any())

    keep = np.ones(len(raw), dtype=bool)
    for day, hour, offline_s in REBOOTS:
        boot = row_at(day, hour)
        if in_event(times[boot]) or in_event(times[boot + 300]):
            raise ValueError("simulated reboot overlaps an event")
        keep[boot : boot + offline_s] = False
        for column, blank_s in BOOT_BLANK_S.items():
            raw.loc[boot : boot + blank_s - 1, column] = np.nan
    for day, hour, minute in TRANSIT_DROPS:
        keep[row_at(day, hour, minute)] = False
    raw = raw.loc[keep].reset_index(drop=True)
    times = times[keep]

    usb = Path(out) / "usb"
    if usb.exists():
        raise ValueError(f"{usb}: มีอยู่แล้ว — ลบเองก่อนซ้อมใหม่")
    usb.mkdir(parents=True)
    marked = raw["label"].to_numpy() == "marked"
    for _, part in raw.groupby(times.date):
        stamp = _stamp(pd.Timestamp(part["timestamp_iso"].iloc[0]))
        _write_raw(part, usb / f"AG_{stamp}_usb.csv")
        # EV: start = แถว marked แรก · end = แถว marked สุดท้าย (เหมือน tasks_sampler.c)
        flags = marked[part.index]
        edges = np.diff(np.concatenate(([0], flags.astype(np.int8), [0])))
        firsts = np.flatnonzero(edges == 1)
        lasts = np.flatnonzero(edges == -1) - 1
        if len(firsts):
            with (usb / f"EV_{stamp}_usb.csv").open(
                "w", encoding="utf-8", newline=""
            ) as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(EVENT_COLUMNS)
                for i, j in zip(firsts, lasts, strict=True):
                    ts = part["timestamp_iso"].to_numpy()
                    writer.writerow([ts[i], ts[j], "marked", ""])
    truth = Path(out) / "truth_events.csv"
    events.to_csv(truth, index=False, encoding="utf-8", lineterminator="\n")
    return {
        "start": start.isoformat(),
        "rows": len(raw),
        "usb": str(usb),
        "truth": str(truth),
    }


def fill(draft_path, truth_path, out_path, tol_s=2.0):
    """แทน TODO ด้วยชื่อจาก truth (จับคู่เวลาเริ่ม ±tol_s) · ห้ามเขียนทับ"""
    out_path = Path(out_path)
    if out_path.exists():
        raise ValueError(f"{out_path}: มีไฟล์อยู่แล้ว — ไม่เขียนทับ")
    draft = load_events(draft_path)
    truth = load_events(truth_path)
    tol = pd.Timedelta(seconds=tol_s)
    rows = []
    for row in draft.itertuples(index=False):
        hit = truth[(truth["start_iso"] - row.start_iso).abs() <= tol]
        if len(hit) != 1:
            raise ValueError(f"no unique truth event for {row.start_iso.isoformat()}")
        rows.append(
            [
                row.start_iso.isoformat(),
                row.end_iso.isoformat(),
                hit["label"].iloc[0],
                NOTE,
            ]
        )
    with out_path.open("x", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(EVENT_COLUMNS)
        writer.writerows(rows)
    return len(rows)


def check_header(header):
    """คอมไพล์ header + harness ของ ML-3 (ml/tests/test_baseline.py::C_CHECK) แล้วรัน"""
    try:
        from .tests.test_baseline import C_CHECK
    except ImportError:
        from tests.test_baseline import C_CHECK
    if shutil.which("gcc") is None:
        raise RuntimeError("ไม่พบ gcc บน PATH")
    header = Path(header).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        shutil.copy(header, tmp / header.name)
        source = tmp / "check.c"
        source.write_text(
            C_CHECK.replace("HEADER", f'"{header.name}"'), encoding="utf-8"
        )
        exe = tmp / "check.exe"
        build = subprocess.run(
            ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", str(source),
             "-o", str(exe), "-lm"],
            cwd=tmp, capture_output=True, text=True, check=False,
        )  # fmt: skip
        if build.returncode != 0:
            return False, build.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    last = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
    return result.returncode == 0 and last == "bad 0", last


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_make = sub.add_parser("make")
    p_make.add_argument("--days", type=int, default=5)
    p_make.add_argument("--seed", type=int, default=42)
    p_make.add_argument("--out", type=Path, required=True)
    p_fill = sub.add_parser("fill")
    p_fill.add_argument("--draft", type=Path, required=True)
    p_fill.add_argument("--truth", type=Path, required=True)
    p_fill.add_argument("--out", type=Path, required=True)
    p_header = sub.add_parser("check-header")
    p_header.add_argument("header", type=Path)
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):  # ข้อความไทยบน pipe cp1252 (ดู ml/qa.py)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    np.random.seed(args.seed if args.command == "make" else 0)  # ไม่ใช้ global RNG
    try:
        if args.command == "make":
            info = make(args.out, args.days, args.seed)
            print(f"{NOTE}\n{info}")
        elif args.command == "fill":
            print(
                f"เติมชื่อ {fill(args.draft, args.truth, args.out)} เหตุการณ์ → {args.out}"
            )
        else:
            ok, last = check_header(args.header)
            print(f"{'PASS' if ok else 'FAIL'}: {last}")
            return 0 if ok else 1
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"dryrun: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
