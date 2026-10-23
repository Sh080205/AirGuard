"""R0: ตรวจคุณภาพไฟล์ raw §7 ก่อนนำไปเทรน — อ่านอย่างเดียว ไม่แก้ ไม่ impute ไม่ลบแถว.

    python -m ml.qa --raw "data/raw/usb_train/AG_*_usb.csv" \
        [--events data/events/<file>.csv] --out data/processed/qa/<วันที่>

ผลลัพธ์ใน --out: qa.json (ทุกตัวเลข · รันซ้ำได้ไฟล์เดิมทุกไบต์) · qa.md (สรุปให้คนอ่าน)
· qa_timeseries.png (ถ้ามี matplotlib) · exit code 0 = PASS/WARN · 1 = FAIL · 2 = อ่านไฟล์ไม่ได้

glob ใน --raw ขยายในสคริปต์เอง เพราะ PowerShell ไม่ขยาย * ให้โปรแกรม Python
เกณฑ์ทุกตัวอยู่ใน QAConfig — ค่าเริ่มต้นเป็นค่าคาดหมาย ต้องยืนยันกับข้อมูลจริง เจ้าของโปรเจกต์ปรับได้
"""

import argparse
import glob
import hashlib
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

try:
    from .load import NUMERIC_COLUMNS, load_events, load_raw
except ImportError:  # `python ml/qa.py`
    from load import NUMERIC_COLUMNS, load_events, load_raw

QA_VERSION = "r0-v1"
CORE_COLUMNS = ("pm2_5", "temp_c", "humidity_pct")  # ชุด feature pm_th


@dataclass(frozen=True)
class QAConfig:
    # แถวห่างกันเกินนี้ = ช่องว่าง · เท่ากับ gap reset 1.5 s ของ FW-7 (CLAUDE.md §10)
    gap_s: float = 1.5
    # รีบูต = คอลัมน์ที่เฟิร์มแวร์ตั้งใจเว้นว่างหลังบูต (PM 30 s · ENS160 ~3 นาที · CO 5 นาที)
    # มีค่าก่อนช่องว่าง แต่ว่างต่อเนื่อง ≥ reboot_min_blank_s หลังช่องว่าง · ถ้ามีค่าทันที
    # ทุกตัว = แถวหายระหว่างทาง (USB/logger) · ดูหลายคอลัมน์เพราะถ้า SNTP ใช้เวลานานกว่า 30 s
    # แถวแรกหลังบูตจะมี PM แล้ว เหลือแต่ ENS160/CO ที่ยังว่าง · ค่าคาดหมาย
    reboot_columns: tuple = ("pm2_5", "tvoc_ppb", "co_ppm")
    reboot_min_blank_s: float = 10.0
    # ช่วงที่ตั้งใจให้ว่างหลังเริ่ม segment (วินาที นับจากแถวแรกหลังบูต)
    warmup_s: dict = field(
        default_factory=lambda: {
            "pm1_0": 30.0,  # PMS_WARMUP_US · firmware/main/tasks_sampler.c
            "pm2_5": 30.0,
            "pm10": 30.0,
            # ENS160 warm-up 3 นาที (ens160_parse.h · DS v1.3 §10.2) · วัดได้ 190 s เมื่อ 21 ต.ค.
            # บวกเผื่อเป็น 240 s — ส่วนเผื่อเป็นสมมติฐาน
            "tvoc_ppb": 240.0,
            "eco2_ppm": 240.0,
            "co_ppm": 300.0,  # CO_WARMUP_US · firmware/components/ze07/include/co_safety.h
            # AHT21 วัดทุก 2 s ไม่มี warm-up ในเฟิร์มแวร์ · เผื่อรอบวัดแรก — สมมติฐาน
            "temp_c": 5.0,
            "humidity_pct": 5.0,
        }
    )
    # ค่าเดิมติดกันนานเท่านี้ (นาที) = ค่าค้าง · AHT21 ซ้ำ 2 แถว / PM ซ้ำ 2–3 แถวเป็นเรื่องปกติ (§7)
    stuck_min: float = 10.0
    # ขอบเขตจากนิยามเท่านั้น (ความเข้มข้นติดลบไม่ได้ · RH อยู่ใน 0–100 %) ไม่ใช่สเปกเซนเซอร์
    # ถ้าจะใช้ช่วงวัดของเซนเซอร์ ต้องเปิด datasheet แต่ละตัวก่อน
    bounds: dict = field(
        default_factory=lambda: {
            "pm1_0": (0.0, None),
            "pm2_5": (0.0, None),
            "pm10": (0.0, None),
            "humidity_pct": (0.0, 100.0),
            "tvoc_ppb": (0.0, None),
            "eco2_ppm": (0.0, None),
            "co_ppm": (0.0, None),
        }
    )
    # ช่วง marked ใน raw กับ event ถือว่าตรงกันเมื่อต้นและปลายห่างกันไม่เกินนี้
    match_tol_s: float = 2.0
    # เกณฑ์ FAIL — ค่าคาดหมาย ต้องยืนยันด้วยการวัด
    max_missing_rows_pct: float = 1.0
    max_single_gap_s: float = 600.0
    max_core_blank_pct: float = 1.0


class QAInputError(ValueError):
    """อ่านไฟล์ input ไม่ได้ — ข้อความบอกชื่อไฟล์ที่ผิด"""


# ------------------------------------------------------------------ input --
def expand_paths(patterns):
    """ขยาย glob เอง (PowerShell ไม่ขยายให้) · เรียงชื่อ · ห้ามซ้ำ · ไม่พบ = error"""
    paths = []
    for pattern in patterns:
        text = str(pattern)
        if glob.has_magic(text):
            found = sorted(glob.glob(text))
            if not found:
                raise QAInputError(f"{text}: ไม่พบไฟล์ที่ตรงกับ pattern")
            paths.extend(Path(p) for p in found)
        else:
            path = Path(text)
            if not path.is_file():
                raise QAInputError(f"{text}: ไม่พบไฟล์")
            paths.append(path)
    if len({p.resolve() for p in paths}) != len(paths):
        raise QAInputError("ไฟล์เดียวกันถูกระบุซ้ำ")
    return paths


def load_checked(paths):
    """load_raw ทั้งชุด · ถ้า error ให้ไล่ทีละไฟล์เพื่อบอกว่าไฟล์ไหนผิด"""
    try:
        return load_raw(paths)
    except ValueError as exc:
        for path in paths:
            try:
                load_raw([path])
            except ValueError as single:
                raise QAInputError(f"{path}: {single}") from single
        # ทุกไฟล์ผ่านเดี่ยว ๆ → ผิดตอนรวม (เช่น timestamp ซ้ำข้ามไฟล์ SD กับ USB)
        raise QAInputError(f"รวมไฟล์ไม่ได้: {exc}") from exc


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------- helpers --
def _runs(mask):
    """ช่วงของ True ที่ติดกัน → list ของ (i0, i1) แบบรวมปลาย"""
    mask = np.asarray(mask, dtype=bool)
    if not mask.any():
        return []
    edges = np.diff(np.concatenate(([0], mask.view(np.int8), [0])))
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1) - 1
    return list(zip(starts.tolist(), ends.tolist()))


def _iso(time):
    return pd.Timestamp(time).isoformat()


def _r(value, digits=4):
    if value is None or (isinstance(value, float) and not np.isfinite(value)):
        return None
    return round(float(value), digits)


# --------------------------------------------------------------- analysis --
def find_gaps(frame, cfg):
    """ช่องว่างระหว่างแถว > gap_s · แยกเป็น reboot หรือ transit (แถวหายระหว่างทาง)"""
    index = frame.index
    if len(index) < 2:
        return []
    diffs = (index[1:] - index[:-1]).total_seconds().to_numpy()
    last = index[-1]
    valid, next_valid = {}, {}
    for column in cfg.reboot_columns:
        valid[column] = frame[column].notna().to_numpy()
        # เวลาของแถวถัดไปที่คอลัมน์นี้มีค่า (รวมแถวตัวเอง) — NaT ถ้าไม่มีจนจบไฟล์
        next_valid[column] = pd.Series(index.where(valid[column])).bfill().to_numpy()
    gaps = []
    for i in np.flatnonzero(diffs > cfg.gap_s).tolist():
        after = i + 1
        blank_after = {}
        for column in cfg.reboot_columns:
            if not valid[column][i]:
                continue  # ว่างตั้งแต่ก่อนช่องว่าง (เช่น CO FAULT) → ใช้แยกรีบูตไม่ได้
            if valid[column][after]:
                blank_s = 0.0
            elif pd.isna(next_valid[column][after]):
                blank_s = (last - index[after]).total_seconds() + 1
            else:
                nxt = pd.Timestamp(next_valid[column][after])
                blank_s = (nxt - index[after]).total_seconds()
            blank_after[column] = _r(blank_s, 3)
        reboot = any(v >= cfg.reboot_min_blank_s for v in blank_after.values())
        gaps.append(
            {
                "after": _iso(index[i]),
                "resume": _iso(index[after]),
                "duration_s": _r(diffs[i], 3),
                "missing_rows": round(float(diffs[i])) - 1,
                "kind": "reboot" if reboot else "transit",
                "blank_after_s": blank_after,
                "_row": after,
            }
        )
    return gaps


def seconds_since_segment_start(frame, gaps):
    """segment เริ่มที่แถวแรกของข้อมูลและหลังช่องว่างแบบ reboot ทุกครั้ง"""
    index = frame.index
    starts = np.zeros(len(index), dtype=bool)
    if len(index):
        starts[0] = True
    for gap in gaps:
        if gap["kind"] == "reboot":
            starts[gap["_row"]] = True
    seg_start = pd.Series(index.where(starts)).ffill()
    return (pd.Series(index) - seg_start).dt.total_seconds().to_numpy()


def blank_stats(frame, since, cfg):
    rows = len(frame)
    out = {}
    for column in NUMERIC_COLUMNS:
        blank = frame[column].isna().to_numpy()
        warm = since < cfg.warmup_s.get(column, 0.0)
        outside = blank & ~warm
        runs = _runs(outside)
        longest = max((j - i + 1 for i, j in runs), default=0)
        out[column] = {
            "blank_rows": int(blank.sum()),
            "blank_in_warmup_rows": int((blank & warm).sum()),
            "blank_outside_warmup_rows": int(outside.sum()),
            "blank_outside_warmup_pct": _r(100.0 * outside.sum() / rows)
            if rows
            else None,
            "longest_blank_run_outside_warmup_rows": int(longest),
        }
    return out


def stuck_runs(frame, cfg):
    """ค่าเดิมติดกันนาน ≥ stuck_min นาที · เทียบเฉพาะแถวที่มีค่า

    ช่องว่างไม่ตัด run — เซนเซอร์ที่ค้างแต่มีแถวว่างแทรกเป็นระยะต้องยังถูกจับได้
    """
    limit = cfg.stuck_min * 60.0
    found = []
    for column in NUMERIC_COLUMNS:
        series = frame[column].dropna()
        index = series.index
        values = series.to_numpy(dtype=float)
        same = values[1:] == values[:-1]
        for i, j in _runs(same):
            start, end = index[i], index[j + 1]
            duration = (end - start).total_seconds()
            if duration >= limit:
                found.append(
                    {
                        "column": column,
                        "start": _iso(start),
                        "end": _iso(end),
                        "duration_s": _r(duration, 3),
                        "value": _r(values[i]),
                    }
                )
    return found


def bounds_violations(frame, cfg):
    out = {}
    for column, (low, high) in sorted(cfg.bounds.items()):
        values = frame[column]
        bad = pd.Series(False, index=frame.index)
        if low is not None:
            bad |= values < low
        if high is not None:
            bad |= values > high
        count = int(bad.sum())
        if count:
            out[column] = {
                "rows": count,
                "first": [_iso(t) for t in frame.index[bad.to_numpy()][:5]],
                "bounds": [low, high],
            }
    return out


def marked_vs_events(frame, events, cfg):
    """ช่วง marked ใน raw · ถ้ามี events ให้จับคู่ต้น/ปลายภายใน match_tol_s"""
    index = frame.index
    runs = []
    # ช่วง marked นับตามลำดับแถว (ช่องว่างกลางช่วงไม่ตัด เพราะสถานะ MARK อยู่ในเฟิร์มแวร์)
    for i, j in _runs(frame["label"].to_numpy() == "marked"):
        runs.append(
            {
                "start": index[i],
                "end": index[j] + pd.Timedelta(seconds=1),
                "rows": j - i + 1,
            }
        )
    tol = pd.Timedelta(seconds=cfg.match_tol_s)
    used = set()
    marked = []
    for run in runs:
        match = None
        if events is not None:
            for k, ev in events.iterrows():
                if k in used:
                    continue
                if (
                    abs(ev["start_iso"] - run["start"]) <= tol
                    and abs(ev["end_iso"] - run["end"]) <= tol
                ):
                    match = k
                    used.add(k)
                    break
        marked.append(
            {
                "start": _iso(run["start"]),
                "end": _iso(run["end"]),
                "rows": run["rows"],
                "event_label": None if match is None else events.loc[match, "label"],
            }
        )
    without_marked = []
    if events is not None:
        first, last = index[0], index[-1]
        for k, ev in events.iterrows():
            if k in used:
                continue
            inside = ev["end_iso"] > first and ev["start_iso"] <= last
            without_marked.append(
                {
                    "start": _iso(ev["start_iso"]),
                    "end": _iso(ev["end_iso"]),
                    "label": ev["label"],
                    "within_raw_range": bool(inside),
                }
            )
    return marked, without_marked


def daily_stats(frame):
    """ต่อวัน (เวลา +07:00): จำนวนแถว · ความครอบคลุม · median/p5/p95 ของ pm_th บนแถว normal"""
    out = {}
    first, last = frame.index[0], frame.index[-1]
    for day, part in frame.groupby(frame.index.date):
        day_start = pd.Timestamp(day).tz_localize(frame.index.tz)
        span_start = max(day_start, first)
        span_end = min(day_start + pd.Timedelta(days=1) - pd.Timedelta(seconds=1), last)
        expected = (span_end - span_start).total_seconds() + 1
        normal = part[part["label"] == "normal"]
        stats = {}
        for column in CORE_COLUMNS:
            values = normal[column].dropna()
            stats[column] = (
                {
                    "median": _r(values.median()),
                    "p5": _r(values.quantile(0.05)),
                    "p95": _r(values.quantile(0.95)),
                }
                if len(values)
                else None
            )
        out[str(day)] = {
            "rows": len(part),
            "coverage_pct": _r(100.0 * len(part) / expected),
            "normal_rows": len(normal),
            "stats_normal": stats,
        }
    return out


def correlation(frame):
    normal = frame.loc[frame["label"] == "normal", list(CORE_COLUMNS)]
    matrix = normal.corr(method="pearson", min_periods=2)
    return {
        row: {col: _r(matrix.loc[row, col]) for col in CORE_COLUMNS}
        for row in CORE_COLUMNS
    }


def verdict(summary, cfg):
    reasons = []
    if summary["missing_rows_pct"] > cfg.max_missing_rows_pct:
        reasons.append(
            (
                "FAIL",
                f"แถวหาย {summary['missing_rows_pct']} % > {cfg.max_missing_rows_pct} %",
            )
        )
    longest = summary["longest_gap_s"]
    if longest is not None and longest > cfg.max_single_gap_s:
        reasons.append(("FAIL", f"ช่องว่างยาวสุด {longest} s > {cfg.max_single_gap_s} s"))
    for column in CORE_COLUMNS:
        pct = summary["blank"][column]["blank_outside_warmup_pct"]
        if pct is not None and pct > cfg.max_core_blank_pct:
            reasons.append(
                (
                    "FAIL",
                    f"{column} ว่างนอกช่วงอุ่นเครื่อง {pct} % > {cfg.max_core_blank_pct} %",
                )
            )
    for column in NUMERIC_COLUMNS:
        if column in CORE_COLUMNS:
            continue
        pct = summary["blank"][column]["blank_outside_warmup_pct"]
        if pct is not None and pct > cfg.max_core_blank_pct:
            reasons.append(
                ("WARN", f"{column} ว่างนอกช่วงอุ่นเครื่อง {pct} % (ไม่อยู่ใน pm_th)")
            )
    reboots = sum(g["kind"] == "reboot" for g in summary["gaps"])
    if reboots:
        reasons.append(("WARN", f"บอร์ดรีบูต {reboots} ครั้ง"))
    # ค่าค้างนับเป็น WARN เฉพาะชุด pm_th · คอลัมน์อื่นความละเอียดต่ำ (เช่น co_ppm 0.1 ppm)
    # นิ่งได้นานในอากาศสะอาด จึงรายงานไว้เฉย ๆ
    core_stuck = sorted({s["column"] for s in summary["stuck"]} & set(CORE_COLUMNS))
    if core_stuck:
        reasons.append(
            ("WARN", f"ค่าค้าง ≥ {cfg.stuck_min:g} นาที: {', '.join(core_stuck)}")
        )
    if summary["out_of_bounds"]:
        reasons.append(
            ("WARN", f"ค่านอกขอบเขตนิยาม: {', '.join(sorted(summary['out_of_bounds']))}")
        )
    unnamed = [m for m in summary["marked"] if m["event_label"] is None]
    if unnamed:
        reasons.append(
            ("WARN", f"ช่วง marked ที่ยังไม่มีชื่อใน events {len(unnamed)} ช่วง (จะเป็น unknown)")
        )
    level = "PASS"
    if any(r[0] == "FAIL" for r in reasons):
        level = "FAIL"
    elif reasons:
        level = "WARN"
    return level, [f"{lvl}: {text}" for lvl, text in reasons]


def analyze(frame, events=None, cfg=None):
    """วิเคราะห์ DataFrame จาก load_raw (ไม่แก้ frame) → dict ที่ serialise เป็น JSON ได้"""
    cfg = cfg or QAConfig()
    if frame.empty:
        raise QAInputError("ไม่มีแถวข้อมูล")
    index = frame.index
    span_s = (index[-1] - index[0]).total_seconds()
    expected = round(span_s) + 1
    gaps = find_gaps(frame, cfg)
    since = seconds_since_segment_start(frame, gaps)
    marked, without_marked = marked_vs_events(frame, events, cfg)
    summary = {
        "start": _iso(index[0]),
        "end": _iso(index[-1]),
        "rows": len(frame),
        "expected_rows_1hz": expected,
        "missing_rows": expected - len(frame),
        "missing_rows_pct": _r(100.0 * (expected - len(frame)) / expected),
        "longest_gap_s": max((g["duration_s"] for g in gaps), default=None),
        "gaps": [{k: v for k, v in g.items() if k != "_row"} for g in gaps],
        "blank": blank_stats(frame, since, cfg),
        "stuck": stuck_runs(frame, cfg),
        "out_of_bounds": bounds_violations(frame, cfg),
        "marked": marked,
        "events_without_marked": without_marked,
        "daily": daily_stats(frame),
        "correlation_pm_th_normal": correlation(frame),
    }
    summary["verdict"], summary["reasons"] = verdict(summary, cfg)
    return summary


# ----------------------------------------------------------------- output --
def render_markdown(report):
    s = report["summary"]
    lines = [
        f"# QA ข้อมูล raw — {s['verdict']}",
        "",
        (
            f"ช่วง {s['start']} → {s['end']} · {s['rows']:,} แถว "
            f"(ควรมี {s['expected_rows_1hz']:,} ที่ 1 Hz · "
            f"หาย {s['missing_rows']:,} = {s['missing_rows_pct']} %)"
        ),
        "",
        "## เหตุผล",
        "",
    ]
    lines += [f"- {r}" for r in s["reasons"]] or ["- ไม่มี"]
    lines += ["", "## ช่องว่าง", ""]
    if s["gaps"]:
        lines += [
            "| หลังแถว | กลับมา | ยาว (s) | แถวหาย | ชนิด |",
            "|---|---|---|---|---|",
        ]
        lines += [
            f"| {g['after']} | {g['resume']} | {g['duration_s']} | {g['missing_rows']} | {g['kind']} |"
            for g in s["gaps"]
        ]
    else:
        lines.append("ไม่มี")
    lines += [
        "",
        "## ค่าว่างต่อคอลัมน์",
        "",
        "| คอลัมน์ | ว่างทั้งหมด | ในช่วงอุ่นเครื่อง | นอกช่วงอุ่นเครื่อง | % นอกช่วง | ว่างติดกันนานสุด (แถว) |",
        "|---|---|---|---|---|---|",
    ]
    for column, b in s["blank"].items():
        lines.append(
            f"| {column} | {b['blank_rows']} | {b['blank_in_warmup_rows']} | "
            f"{b['blank_outside_warmup_rows']} | {b['blank_outside_warmup_pct']} | "
            f"{b['longest_blank_run_outside_warmup_rows']} |"
        )
    lines += ["", "## ค่าค้าง", ""]
    core = [x for x in s["stuck"] if x["column"] in CORE_COLUMNS]
    if core:
        lines += ["| คอลัมน์ | เริ่ม | จบ | นาน (s) | ค่า |", "|---|---|---|---|---|"]
        lines += [
            f"| {x['column']} | {x['start']} | {x['end']} | {x['duration_s']} | {x['value']} |"
            for x in core
        ]
    else:
        lines.append("ชุด pm_th: ไม่มี")
    others = {}
    for x in s["stuck"]:
        if x["column"] not in CORE_COLUMNS:
            others[x["column"]] = others.get(x["column"], 0) + 1
    if others:
        lines += [
            "",
            "คอลัมน์นอก pm_th (ไม่นับเป็น WARN · รายละเอียดใน qa.json): "
            + ", ".join(f"{c} {n} ช่วง" for c, n in sorted(others.items())),
        ]
    lines += ["", "## ช่วง marked", ""]
    if s["marked"]:
        lines += ["| เริ่ม | จบ | แถว | ชื่อใน events |", "|---|---|---|---|"]
        lines += [
            f"| {m['start']} | {m['end']} | {m['rows']} | {m['event_label'] or '— (unknown)'} |"
            for m in s["marked"]
        ]
    else:
        lines.append("ไม่มี")
    if s["events_without_marked"]:
        lines += ["", "event ที่ไม่มีช่วง marked คู่ (จดด้วยมือ หรือเวลาไม่ตรง):", ""]
        lines += [
            f"- {e['start']} → {e['end']} `{e['label']}`"
            + ("" if e["within_raw_range"] else " (อยู่นอกช่วงข้อมูล)")
            for e in s["events_without_marked"]
        ]
    lines += [
        "",
        "## รายวัน (แถว normal)",
        "",
        "| วัน | แถว | ครอบคลุม % | pm2_5 med [p5–p95] | temp_c med [p5–p95] | humidity_pct med [p5–p95] |",
        "|---|---|---|---|---|---|",
    ]
    for day, d in s["daily"].items():
        cells = []
        for column in CORE_COLUMNS:
            st = d["stats_normal"][column]
            cells.append(
                "—" if st is None else f"{st['median']} [{st['p5']}–{st['p95']}]"
            )
        lines.append(
            f"| {day} | {d['rows']} | {d['coverage_pct']} | " + " | ".join(cells) + " |"
        )
    lines += ["", "## correlation (Pearson · แถว normal · ค่าดิบ)", ""]
    lines += [
        "| | " + " | ".join(CORE_COLUMNS) + " |",
        "|---" * (len(CORE_COLUMNS) + 1) + "|",
    ]
    for row, cols in s["correlation_pm_th_normal"].items():
        lines.append(
            f"| {row} | " + " | ".join(str(cols[c]) for c in CORE_COLUMNS) + " |"
        )
    lines += [
        "",
        "## ไฟล์ input",
        "",
        *[f"- `{f['name']}` sha256 `{f['sha256'][:16]}…`" for f in report["inputs"]],
        "",
        f"รูป: {report['plot']}",
        "",
        "temp_c / humidity_pct คือค่ารอบโมดูลที่โดนฮีตเตอร์ ENS160 ไม่ใช่ค่าห้อง",
        "",
    ]
    return "\n".join(lines)


def plot(frame, summary, path):
    """time series 5 แผง แรเงาช่วง marked (ส้ม) และช่องว่าง (เทา) · ข้ามถ้าไม่มี matplotlib"""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return "ข้าม: ไม่ได้ติดตั้ง matplotlib"
    columns = ("pm2_5", "temp_c", "humidity_pct", "tvoc_ppb", "co_ppm")
    fig, axes = plt.subplots(len(columns), 1, figsize=(14, 11), sharex=True)
    times = frame.index.tz_localize(None)
    for ax, column in zip(axes, columns):
        ax.plot(times, frame[column].to_numpy(), lw=0.6)
        ax.set_ylabel(column)
        for m in summary["marked"]:
            ax.axvspan(
                pd.Timestamp(m["start"]).tz_localize(None),
                pd.Timestamp(m["end"]).tz_localize(None),
                color="tab:orange",
                alpha=0.25,
            )
        for g in summary["gaps"]:
            ax.axvspan(
                pd.Timestamp(g["after"]).tz_localize(None),
                pd.Timestamp(g["resume"]).tz_localize(None),
                color="0.5",
                alpha=0.4,
            )
    axes[0].set_title(f"{summary['start']} → {summary['end']} · {summary['verdict']}")
    fig.tight_layout()
    fig.savefig(path, dpi=110, metadata={"Software": None})
    plt.close(fig)
    return path.name


def run(raw_patterns, out, events_path=None, cfg=None, make_plot=True):
    cfg = cfg or QAConfig()
    paths = expand_paths(raw_patterns)
    frame = load_checked(paths)
    events = None
    if events_path is not None:
        try:
            events = load_events(events_path)
        except (OSError, ValueError) as exc:
            raise QAInputError(f"{events_path}: {exc}") from exc
    summary = analyze(frame, events, cfg)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    report = {
        "qa_version": QA_VERSION,
        "inputs": [{"name": p.name, "sha256": sha256(p)} for p in paths],
        "events": None
        if events_path is None
        else {"name": Path(events_path).name, "sha256": sha256(events_path)},
        "config": asdict(cfg),
        "summary": summary,
    }
    report["plot"] = (
        plot(frame, summary, out / "qa_timeseries.png")
        if make_plot
        else "ข้าม: --no-plot"
    )
    (out / "qa.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=1, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (out / "qa.md").write_text(render_markdown(report), encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--raw", nargs="+", required=True, help="ไฟล์ raw §7 (รับ glob)")
    parser.add_argument(
        "--events", type=Path, help="events CSV (start_iso,end_iso,label,note)"
    )
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stuck-min", type=float, default=QAConfig.stuck_min)
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args(argv)
    # ข้อความเป็นภาษาไทย · ถ้า stdout เป็น pipe ที่ใช้ cp1252 จะ crash → แสดงเป็น \uXXXX แทน
    # (qa.json/qa.md เขียนเป็น UTF-8 เสมอ ไม่ได้รับผล)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    cfg = QAConfig(stuck_min=args.stuck_min)
    try:
        report = run(args.raw, args.out, args.events, cfg, make_plot=not args.no_plot)
    except QAInputError as exc:
        print(f"qa: {exc}", file=sys.stderr)
        return 2
    summary = report["summary"]
    print(
        f"{summary['verdict']} · {summary['rows']:,} แถว · หาย {summary['missing_rows_pct']} % "
        f"· ช่องว่าง {len(summary['gaps'])} · {args.out / 'qa.md'}"
    )
    for reason in summary["reasons"]:
        print(f"  {reason}")
    return 1 if summary["verdict"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
