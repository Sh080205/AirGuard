"""Synthetic fixtures for ML-1, never evidence of sensor accuracy.

All event intervals are half-open [start_iso, end_iso). Raw labels follow
CLAUDE.md §7: normal/marked; anomaly names live only in events.csv.
"""

import argparse
import csv
from dataclasses import asdict, dataclass, fields
from datetime import timedelta
from pathlib import Path

import numpy as np
import pandas as pd

try:
    from .load import EVENT_COLUMNS, INTEGER_COLUMNS, NUMERIC_COLUMNS, RAW_COLUMNS
except ImportError:
    from load import EVENT_COLUMNS, INTEGER_COLUMNS, NUMERIC_COLUMNS, RAW_COLUMNS

DAY_S = 86400  # Time unit, not an assumed sensor measurement period.


@dataclass(frozen=True)
class SynthConfig:
    """สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง (every parameter).

    No parameter represents calibrated hardware, accuracy or measured timing.
    Cooking and decorrelation share the PM profile and amplitude by design.
    Offsets are seconds from each generated day's start, not wall-clock hours.
    """

    default_start: str = "2026-09-23T00:00:00+07:00"
    missing_rate: float = 0.002
    pm_base: float = 12.0
    pm_daily_amplitude: float = 4.0
    pm_noise_std: float = 0.5
    pm1_ratio: float = 0.7
    pm10_ratio: float = 1.3
    temp_base: float = 26.0
    temp_daily_amplitude: float = 2.0
    temp_noise_std: float = 0.1
    humidity_base: float = 60.0
    humidity_daily_amplitude: float = 6.0
    humidity_noise_std: float = 0.3
    th_hold_s: int = 2  # Synthetic sample/hold to match §7 repeated T/RH rows.
    tvoc_base: float = 120.0
    tvoc_daily_amplitude: float = 20.0
    tvoc_noise_std: float = 3.0
    eco2_intercept: float = 400.0
    eco2_tvoc_gain: float = 0.8
    eco2_noise_std: float = 1.0
    co_base: float = 0.4
    co_daily_amplitude: float = 0.06
    co_noise_std: float = 0.01
    cooking_offsets_s: tuple[int, ...] = (43200, 64800)
    cooking_duration_s: int = 3600
    pm_event_rise: float = 10.0
    cooking_temp_rise: float = 1.5
    cooking_humidity_rise: float = 3.0
    decorrelation_offset_s: int = 50400
    decorrelation_duration_s: int = 600
    spike_offset_s: int = 72000
    spike_duration_s: int = 60
    spike_pm_rise: float = 100.0
    drift_offset_s: int = 79200
    drift_duration_s: int = 600
    drift_tvoc_rise: float = 150.0

    def validate(self):
        for field in fields(self):
            value = getattr(self, field.name)
            if isinstance(value, (float, int)) and (
                not np.isfinite(value) or value < 0
            ):
                raise ValueError(f"{field.name} must be finite and nonnegative")
        if not 0 <= self.missing_rate <= 1:
            raise ValueError("missing_rate must be in [0, 1]")
        if not isinstance(self.th_hold_s, int) or self.th_hold_s < 1:
            raise ValueError("th_hold_s must be a positive integer")
        schedule = [
            (offset, self.cooking_duration_s) for offset in self.cooking_offsets_s
        ] + [
            (self.decorrelation_offset_s, self.decorrelation_duration_s),
            (self.spike_offset_s, self.spike_duration_s),
            (self.drift_offset_s, self.drift_duration_s),
        ]
        end = 0
        for offset, duration in sorted(schedule):
            if (
                not isinstance(offset, int)
                or not isinstance(duration, int)
                or duration < 1
                or offset < end
                or offset + duration > DAY_S
            ):
                raise ValueError(
                    "event schedules require positive integer durations, "
                    "no overlaps and intervals within a day"
                )
            end = offset + duration


def generate(days, seed, start=None, cfg=None):
    """Return raw_df and anomaly-only events_df at exactly one row per second.

    Daily sine waves make normal T/RH anticorrelated; a shared positive
    cooking pulse adds PM/T/RH together. Decorrelation reuses the PM recipe
    while holding T/RH steady. Drift is a linear TVOC ramp; eCO2 follows TVOC.
    Missingness is independently sampled per numeric cell after rounding.
    """
    cfg = SynthConfig() if cfg is None else cfg
    cfg.validate()
    if not np.isfinite(days) or days <= 0 or not float(days * DAY_S).is_integer():
        raise ValueError("days must describe a positive whole number of seconds")
    start = pd.Timestamp(cfg.default_start if start is None else start)
    if pd.isna(start) or start.utcoffset() != timedelta(hours=7):
        raise ValueError("start requires timezone +07:00")
    if start.microsecond or start.nanosecond:
        raise ValueError("start must be aligned to a whole second")
    n = int(days * DAY_S)
    rng = np.random.default_rng(seed)
    times = pd.date_range(start, periods=n, freq="s")
    phase = np.arange(n, dtype=float) * (2 * np.pi / DAY_S)
    daily = np.sin(phase)
    pm_noise = rng.normal(0, cfg.pm_noise_std, n)
    pm = cfg.pm_base + cfg.pm_daily_amplitude * daily + pm_noise
    temp = cfg.temp_base + cfg.temp_daily_amplitude * daily
    temp += rng.normal(0, cfg.temp_noise_std, n)
    rh = cfg.humidity_base - cfg.humidity_daily_amplitude * daily
    rh += rng.normal(0, cfg.humidity_noise_std, n)
    tvoc = cfg.tvoc_base + cfg.tvoc_daily_amplitude * daily
    tvoc += rng.normal(0, cfg.tvoc_noise_std, n)
    co = cfg.co_base + cfg.co_daily_amplitude * daily
    co += rng.normal(0, cfg.co_noise_std, n)
    labels = np.full(n, "normal", dtype="U6")
    events = []

    def interval(day, offset, duration):
        left = day + offset
        return left, min(left + duration, n)

    def annotate(left, right, label):
        labels[left:right] = "marked"
        events.append(
            {
                "start_iso": times[left].isoformat(),
                "end_iso": (start + pd.Timedelta(seconds=right)).isoformat(),
                "label": label,
                "note": "สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง; "
                "interval [start,end); not sensor accuracy evidence",
            }
        )

    for day in range(0, n, DAY_S):
        for offset in cfg.cooking_offsets_s:
            left, right = interval(day, offset, cfg.cooking_duration_s)
            if left >= n:
                continue
            pulse = np.sin(np.pi * np.arange(right - left) / cfg.cooking_duration_s)
            pm[left:right] = (
                cfg.pm_base + cfg.pm_event_rise * pulse + pm_noise[left:right]
            )
            temp[left:right] += cfg.cooking_temp_rise * pulse
            rh[left:right] += cfg.cooking_humidity_rise * pulse
        left, right = interval(
            day, cfg.decorrelation_offset_s, cfg.decorrelation_duration_s
        )
        if left < n:
            pulse = np.sin(
                np.pi * np.arange(right - left) / cfg.decorrelation_duration_s
            )
            pm[left:right] = (
                cfg.pm_base + cfg.pm_event_rise * pulse + pm_noise[left:right]
            )
            temp[left:right], rh[left:right] = temp[left], rh[left]
            annotate(left, right, "decorrelation")
        left, right = interval(day, cfg.spike_offset_s, cfg.spike_duration_s)
        if left < n:
            pm[left:right] += cfg.spike_pm_rise
            annotate(left, right, "spike")
        left, right = interval(day, cfg.drift_offset_s, cfg.drift_duration_s)
        if left < n:
            tvoc[left:right] += cfg.drift_tvoc_rise * (
                np.arange(right - left) / cfg.drift_duration_s
            )
            annotate(left, right, "drift")

    # All PM bands derive from one synthetic particle signal, and eCO2 from
    # rounded TVOC. These assumptions express dependence, not sensor calibration.
    pm = np.maximum(pm, 0)
    tvoc = np.rint(np.maximum(tvoc, 0))
    eco2 = cfg.eco2_intercept + cfg.eco2_tvoc_gain * tvoc
    eco2 += rng.normal(0, cfg.eco2_noise_std, n)
    hold = np.arange(n) // cfg.th_hold_s * cfg.th_hold_s
    raw = pd.DataFrame(
        {
            "timestamp_iso": [time.isoformat() for time in times],
            "pm1_0": np.rint(pm * cfg.pm1_ratio),
            "pm2_5": np.rint(pm),
            "pm10": np.rint(pm * cfg.pm10_ratio),
            "temp_c": np.round(temp[hold], 1),
            "humidity_pct": np.round(np.clip(rh[hold], 0, 100), 1),
            "tvoc_ppb": tvoc,
            "eco2_ppm": np.rint(np.maximum(eco2, 0)),
            "co_ppm": np.round(np.maximum(co, 0), 1),
            "label": labels,
        },
        columns=RAW_COLUMNS,
    )
    missing = rng.random((n, len(NUMERIC_COLUMNS))) < cfg.missing_rate
    raw.loc[:, list(NUMERIC_COLUMNS)] = raw.loc[:, list(NUMERIC_COLUMNS)].mask(missing)
    events = pd.DataFrame(events, columns=EVENT_COLUMNS)
    return raw, events


def write_csv(raw_df, events_df, out):
    """Write deterministic UTF-8/LF CSV with integer/one-decimal channel formats.

    Generated data may only be written below this repo's data/processed/.
    """
    out = Path(out).resolve()
    allowed = Path(__file__).resolve().parents[1] / "data" / "processed"
    if not out.is_relative_to(allowed.resolve()):
        raise ValueError("synthetic output must be under data/processed/")
    if list(raw_df.columns) != list(RAW_COLUMNS):
        raise ValueError("raw header mismatch")
    if list(events_df.columns) != list(EVENT_COLUMNS):
        raise ValueError("events header mismatch")
    out.mkdir(parents=True, exist_ok=True)
    raw_path, events_path = out / "raw.csv", out / "events.csv"
    with raw_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(RAW_COLUMNS)
        for row in raw_df.itertuples(index=False, name=None):
            formatted = [row[0]]
            for column, value in zip(NUMERIC_COLUMNS, row[1:-1], strict=True):
                formatted.append(
                    ""
                    if pd.isna(value)
                    else format(value, ".0f" if column in INTEGER_COLUMNS else ".1f")
                )
            writer.writerow([*formatted, row[-1]])
    events_df.to_csv(events_path, index=False, encoding="utf-8", lineterminator="\n")
    return raw_path, events_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--days", type=float, default=7)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--start", default=None)
    parser.add_argument(
        "--missing-rate", type=float, default=SynthConfig().missing_rate
    )
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    cfg = SynthConfig(missing_rate=args.missing_rate)
    raw, events = generate(args.days, args.seed, args.start, cfg)
    raw_path, events_path = write_csv(raw, events, args.out)
    print("สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง; not evidence of sensor accuracy")
    print(
        f"rows={len(raw)}; events={len(events)}; "
        f"anomaly_points={(raw.label == 'marked').sum()}; "
        f"anomaly_rate={(raw.label == 'marked').mean():.6%}"
    )
    print(f"event_counts={events.label.value_counts().sort_index().to_dict()}")
    print(f"numeric_missing_cells={raw[list(NUMERIC_COLUMNS)].isna().sum().sum()}")
    print(f"config={asdict(cfg)}")
    print(f"raw={raw_path}\nevents={events_path}")


if __name__ == "__main__":
    main()
