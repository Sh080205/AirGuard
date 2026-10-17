"""Compare AHT21 temp_c / humidity_pct with the ENS160 heater off and on.

FW-5a step 3: the ENS160 heater sits on the same module as the AHT21, so it
may warm the T/RH sensor. Record >= 30 min with the ENS160 in DEEP SLEEP
(build with -DENS160_ENABLE=0) and >= 30 min with it running, then:

    python tools\\heater_compare.py --csv data\\raw\\usb\\AG_..._usb.csv ^
        --phase off  2026-10-16T19:00:00+07:00 2026-10-16T19:35:00+07:00 ^
        --phase on   2026-10-16T19:37:00+07:00 2026-10-16T20:12:00+07:00 ^
        --phase off2 2026-10-16T20:14:00+07:00 2026-10-16T20:49:00+07:00 ^
        --compare on=off,off2

- Each phase drops its first --settle-min minutes (default 10) because the
  module needs time to heat up / cool down after a flash; only the rest is
  averaged. ค่า 10 นาทีเป็นค่าคาดหมาย — ดู slope ของแต่ละช่วงประกอบ.
- --compare TEST=BASE[,BASE2]: TEST mean minus the mean of the BASE means.
  Two baselines before and after (off / on / off) cancel a room temperature
  drift that is linear in time. Default: every phase against the first one.
- --ref FILE (optional): reference thermometer readings, CSV with header
  time_iso,temp_c,humidity_pct (humidity may be empty). Each reading is
  paired with the AHT21 row of the same second (or nearest within
  --ref-window-s); the per-phase mean of (AHT21 - reference) removes room
  drift completely, so its difference between phases is the heater effect.

Standard library only; reads the files written by serial_logger.py
(CLAUDE.md §7 contract). Missing values (empty fields) are skipped.
"""

import argparse
import csv
import math
import sys
from datetime import datetime, timedelta

T_COL = "temp_c"
RH_COL = "humidity_pct"


def parse_time(text):
    """ISO 8601 with offset, e.g. 2026-10-16T19:00:00+07:00."""
    t = datetime.fromisoformat(text.strip())
    if t.tzinfo is None:
        raise ValueError(f"time needs an offset like +07:00: {text!r}")
    return t


def to_float(text):
    text = (text or "").strip()
    return float(text) if text else None


def load_rows(paths):
    """[(datetime, temp or None, rh or None)] sorted by time."""
    rows = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            for rec in csv.DictReader(f):
                rows.append((parse_time(rec["timestamp_iso"]),
                             to_float(rec.get(T_COL)), to_float(rec.get(RH_COL))))
    rows.sort(key=lambda r: r[0])
    return rows


def load_ref(path):
    refs = []
    with open(path, newline="", encoding="utf-8") as f:
        for rec in csv.DictReader(f):
            refs.append((parse_time(rec["time_iso"]),
                         to_float(rec.get(T_COL)), to_float(rec.get(RH_COL))))
    refs.sort(key=lambda r: r[0])
    return refs


def describe(values):
    """n, mean, sample std, min, max of a list of floats (n may be 0)."""
    n = len(values)
    if n == 0:
        return {"n": 0, "mean": None, "std": None, "min": None, "max": None}
    mean = sum(values) / n
    std = math.sqrt(sum((v - mean) ** 2 for v in values) / (n - 1)) if n > 1 else 0.0
    return {"n": n, "mean": mean, "std": std, "min": min(values), "max": max(values)}


def slope_per_hour(points):
    """Least-squares slope of value vs time in units per hour; None if < 2
    points or all at one time. points: [(datetime, value)]."""
    if len(points) < 2:
        return None
    t0 = points[0][0]
    xs = [(t - t0).total_seconds() / 3600.0 for t, _ in points]
    ys = [v for _, v in points]
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def phase_rows(rows, start, end, settle):
    """Rows with start + settle <= time <= end."""
    first = start + settle
    return [r for r in rows if first <= r[0] <= end]


def nearest_row(rows, when, window):
    best = None
    for r in rows:
        d = abs((r[0] - when).total_seconds())
        if d <= window.total_seconds() and (best is None or d < best[0]):
            best = (d, r)
    return None if best is None else best[1]


def summarize_phase(rows, refs, ref_window):
    """Stats for one phase. refs: reference readings inside the same window."""
    temps = [(t, v) for t, v, _ in rows if v is not None]
    rhs = [(t, v) for t, _, v in rows if v is not None]
    out = {
        "rows": len(rows),
        "T": describe([v for _, v in temps]),
        "RH": describe([v for _, v in rhs]),
        "T_slope_h": slope_per_hour(temps),
        "RH_slope_h": slope_per_hour(rhs),
        "T_offset": describe([]),
        "RH_offset": describe([]),
    }
    if refs:
        dt, drh = [], []
        for when, rt, rrh in refs:
            r = nearest_row(rows, when, ref_window)
            if r is None:
                continue
            if rt is not None and r[1] is not None:
                dt.append(r[1] - rt)
            if rrh is not None and r[2] is not None:
                drh.append(r[2] - rrh)
        out["T_offset"] = describe(dt)
        out["RH_offset"] = describe(drh)
    return out


def compare(summaries, test, bases, key):
    """summaries[test][key].mean minus the mean of the bases' means; None if
    any of them has no data."""
    tm = summaries[test][key]["mean"]
    bm = [summaries[b][key]["mean"] for b in bases]
    if tm is None or any(m is None for m in bm):
        return None
    return tm - sum(bm) / len(bm)


def parse_compare(text, names):
    if "=" not in text:
        raise ValueError(f"--compare needs TEST=BASE[,BASE2]: {text!r}")
    test, bases = text.split("=", 1)
    bases = [b for b in bases.split(",") if b]
    for name in [test] + bases:
        if name not in names:
            raise ValueError(f"--compare names unknown phase {name!r}")
    if not bases:
        raise ValueError(f"--compare has no baseline: {text!r}")
    return test, bases


def fmt(v, digits=2):
    return "-" if v is None else f"{v:.{digits}f}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--csv", nargs="+", required=True, help="AG_*_usb.csv file(s)")
    ap.add_argument("--phase", nargs=3, action="append", required=True,
                    metavar=("NAME", "START", "END"), help="ISO times with +07:00")
    ap.add_argument("--compare", action="append", default=[], metavar="TEST=BASE[,BASE2]")
    ap.add_argument("--settle-min", type=float, default=10.0)
    ap.add_argument("--ref", help="reference thermometer CSV: time_iso,temp_c,humidity_pct")
    ap.add_argument("--ref-window-s", type=float, default=30.0)
    args = ap.parse_args(argv)

    names = [p[0] for p in args.phase]
    if len(set(names)) != len(names):
        ap.error("phase names must be unique")
    try:
        rows = load_rows(args.csv)
        refs = load_ref(args.ref) if args.ref else []
        phases = [(n, parse_time(s), parse_time(e)) for n, s, e in args.phase]
        comps = [parse_compare(c, names) for c in args.compare]
    except (OSError, KeyError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if not comps:
        comps = [(n, [names[0]]) for n in names[1:]]

    settle = timedelta(minutes=args.settle_min)
    window = timedelta(seconds=args.ref_window_s)
    summaries = {}
    for name, start, end in phases:
        if end <= start + settle:
            print(f"error: phase {name} is not longer than --settle-min", file=sys.stderr)
            return 2
        sel = phase_rows(rows, start, end, settle)
        sel_refs = [r for r in refs if start + settle <= r[0] <= end]
        summaries[name] = summarize_phase(sel, sel_refs, window)

    print(f"settle: first {args.settle_min:g} min of each phase dropped")
    print("phase   rows  T mean  T std  T min  T max  T slope/h | RH mean RH std  RH min RH max RH slope/h"
          + ("  | AHT-ref T (n)  AHT-ref RH (n)" if refs else ""))
    for name, _, _ in phases:
        s = summaries[name]
        line = (f"{name:<7}{s['rows']:>5}  {fmt(s['T']['mean']):>6} {fmt(s['T']['std'], 3):>6}"
                f" {fmt(s['T']['min'], 1):>6} {fmt(s['T']['max'], 1):>6} {fmt(s['T_slope_h']):>9} |"
                f" {fmt(s['RH']['mean']):>7} {fmt(s['RH']['std'], 3):>6} {fmt(s['RH']['min'], 1):>6}"
                f" {fmt(s['RH']['max'], 1):>6} {fmt(s['RH_slope_h']):>10}")
        if refs:
            line += (f"  | {fmt(s['T_offset']['mean']):>7} ({s['T_offset']['n']})"
                     f"  {fmt(s['RH_offset']['mean']):>7} ({s['RH_offset']['n']})")
        print(line)
        if s["T"]["n"] == 0:
            print(f"  warning: phase {name} has no temp_c rows in its window")

    print("\ndifferences (TEST minus mean of BASE):")
    for test, bases in comps:
        line = (f"  {test} - mean({','.join(bases)}): dT = {fmt(compare(summaries, test, bases, 'T'))} C"
                f"  dRH = {fmt(compare(summaries, test, bases, 'RH'))} %RH")
        if refs:
            line += (f" | vs reference: dT = {fmt(compare(summaries, test, bases, 'T_offset'))} C"
                     f"  dRH = {fmt(compare(summaries, test, bases, 'RH_offset'))} %RH")
        print(line)
    # ASCII only: a Thai Windows console (cp874) cannot print § or ±
    print("\nAHT21 accuracy (CLAUDE.md sec. 5): T +-0.3 C, RH +-2 %RH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
