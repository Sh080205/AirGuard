"""R1: สร้างร่างและตรวจไฟล์ events (ground truth) ก่อนส่งให้ ml.baseline.

    python -m ml.events draft --ev "data/raw/usb_train/EV_*_usb.csv" \
        --out data/events/2026-10_train.csv
    python -m ml.events check --events data/events/2026-10_train.csv \
        --raw "data/raw/usb_train/AG_*_usb.csv" --ev "data/raw/usb_train/EV_*_usb.csv" \
        [--allowed incense,dusting,cooking,other] [--t1 ... --t2 ... --gap-s 60]

ช่วงเวลาของ EV row จากเฟิร์มแวร์: start = แถว marked แรก · end = แถว marked **สุดท้าย**
(tasks_sampler.c `mark_last`) แต่ events ใน ML-2 เป็นช่วงครึ่งเปิด [start, end) → `draft`
ใส่ end + 1 s เพื่อให้แถว marked สุดท้ายอยู่ในเหตุการณ์ด้วย

ไม่แปลงชื่อเหตุการณ์จาก note อัตโนมัติ (ml/METRICS.md) — คนต้องเติม label เอง
exit code: 0 = ไม่มี error · 1 = มี error · 2 = อ่านไฟล์ไม่ได้
"""

import argparse
import csv
import sys
from itertools import pairwise
from pathlib import Path

import pandas as pd

try:
    from .baseline import check_event_boundaries
    from .load import EVENT_COLUMNS, load_events
    from .qa import QAConfig, QAInputError, expand_paths, load_checked, marked_vs_events
except ImportError:  # `python ml/events.py`
    from baseline import check_event_boundaries
    from load import EVENT_COLUMNS, load_events
    from qa import QAConfig, QAInputError, expand_paths, load_checked, marked_vs_events

DEFAULT_ALLOWED = ("incense", "dusting", "cooking", "other")
TODO_LABEL = "TODO"
ONE_S = pd.Timedelta(seconds=1)


def _iso(time):
    return pd.Timestamp(time).isoformat()


def load_ev(patterns):
    """อ่าน EV_*.csv ทุกไฟล์ (label = marked · note ว่าง) → start/end ตามที่เฟิร์มแวร์เขียน"""
    frames = []
    for path in expand_paths(patterns):
        try:
            frames.append(load_events(path))
        except ValueError as exc:
            raise QAInputError(f"{path}: {exc}") from exc
    if not frames:
        return pd.DataFrame(columns=list(EVENT_COLUMNS))
    ev = pd.concat(frames, ignore_index=True)
    return ev.sort_values("start_iso").reset_index(drop=True)


def draft(ev_patterns, out):
    """ร่าง events จาก EV rows: end + 1 s · label = TODO · ห้ามเขียนทับไฟล์ที่มีอยู่"""
    out = Path(out)
    if out.exists():
        raise QAInputError(f"{out}: มีไฟล์อยู่แล้ว — ไม่เขียนทับ ground truth")
    ev = load_ev(ev_patterns)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("x", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(EVENT_COLUMNS)
        for row in ev.itertuples(index=False):
            writer.writerow(
                [_iso(row.start_iso), _iso(row.end_iso + ONE_S), TODO_LABEL, ""]
            )
    return len(ev)


def check(
    events_path,
    raw_patterns=None,
    ev_patterns=None,
    allowed=DEFAULT_ALLOWED,
    t1=None,
    t2=None,
    gap_s=60.0,
    tol_s=QAConfig.match_tol_s,
):
    """ตรวจไฟล์ events → {"errors": [...], "warnings": [...], ...}"""
    try:
        events = load_events(events_path)
    except (OSError, ValueError) as exc:
        raise QAInputError(f"{events_path}: {exc}") from exc
    errors, warnings = [], []
    tol = pd.Timedelta(seconds=tol_s)

    def where(row):
        return f"{_iso(row.start_iso)} → {_iso(row.end_iso)} `{row.label}`"

    for row in events.itertuples(index=False):
        if row.label not in allowed:
            errors.append(f"label ไม่อยู่ในรายการที่อนุญาต {list(allowed)}: {where(row)}")
        if not str(row.note).strip():
            errors.append(f"note ว่าง (ต้องจดบริบท): {where(row)}")
    ordered = list(events.itertuples(index=False))
    for prev, cur in pairwise(ordered):
        if cur.start_iso < prev.end_iso:
            errors.append(f"เหตุการณ์ซ้อนกัน: {where(prev)} กับ {where(cur)}")

    manual = []
    if ev_patterns:
        ev = load_ev(ev_patterns)
        used = set()
        for e in ev.itertuples(index=False):
            hit = None
            for k, row in enumerate(ordered):
                if k in used:
                    continue
                if (
                    abs(row.start_iso - e.start_iso) <= tol
                    and abs(row.end_iso - (e.end_iso + ONE_S)) <= tol
                ):
                    hit = k
                    break
            if hit is None:
                errors.append(
                    f"MARK ใน EV ยังไม่ได้ลงใน events: "
                    f"{_iso(e.start_iso)} → {_iso(e.end_iso + ONE_S)}"
                )
            else:
                used.add(hit)
        manual = [where(row) for k, row in enumerate(ordered) if k not in used]
        if manual:
            warnings.append(
                f"เหตุการณ์ที่ไม่มี MARK คู่ {len(manual)} รายการ (จดด้วยมือ?) — "
                "เวลาไม่แม่นเท่า MARK"
            )

    if raw_patterns:
        frame = load_checked(expand_paths(raw_patterns))
        marked, without = marked_vs_events(frame, events, QAConfig(match_tol_s=tol_s))
        for m in marked:
            if m["event_label"] is None:
                errors.append(
                    f"ช่วง marked ใน raw ไม่มีใน events (จะเป็น unknown): "
                    f"{m['start']} → {m['end']}"
                )
        outside = [e for e in without if not e["within_raw_range"]]
        if outside:
            warnings.append(f"เหตุการณ์อยู่นอกช่วงข้อมูล raw {len(outside)} รายการ")

    if t1 is not None and t2 is not None:
        try:
            check_event_boundaries(events, t1, t2, gap_s)
        except ValueError as exc:
            errors.append(f"คร่อมขอบ t1/t2 หรือ gap: {exc}")

    counts = events["label"].value_counts().sort_index()
    return {
        "events": len(events),
        "by_label": {str(k): int(v) for k, v in counts.items()},
        "manual": manual,
        "errors": errors,
        "warnings": warnings,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_draft = sub.add_parser("draft", help="ร่าง events จาก EV_*.csv")
    p_draft.add_argument("--ev", nargs="+", required=True)
    p_draft.add_argument("--out", type=Path, required=True)
    p_check = sub.add_parser("check", help="ตรวจไฟล์ events")
    p_check.add_argument("--events", type=Path, required=True)
    p_check.add_argument("--raw", nargs="+")
    p_check.add_argument("--ev", nargs="+")
    p_check.add_argument("--allowed", default=",".join(DEFAULT_ALLOWED))
    p_check.add_argument("--t1")
    p_check.add_argument("--t2")
    p_check.add_argument("--gap-s", type=float, default=60.0)
    args = parser.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):  # ข้อความไทยบน pipe cp1252 (ดู ml/qa.py)
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    try:
        if args.command == "draft":
            n = draft(args.ev, args.out)
            print(f"ร่าง {n} เหตุการณ์ → {args.out} · แก้ label {TODO_LABEL} และเติม note")
            return 0
        if (args.t1 is None) != (args.t2 is None):
            parser.error("--t1 กับ --t2 ต้องใส่คู่กัน")
        result = check(
            args.events,
            args.raw,
            args.ev,
            tuple(x for x in args.allowed.split(",") if x),
            args.t1,
            args.t2,
            args.gap_s,
        )
    except QAInputError as exc:
        print(f"events: {exc}", file=sys.stderr)
        return 2
    print(f"{result['events']} เหตุการณ์ · {result['by_label']}")
    for text in result["errors"]:
        print(f"ERROR: {text}")
    for text in result["warnings"]:
        print(f"WARN: {text}")
    print("OK" if not result["errors"] else f"{len(result['errors'])} error")
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
