"""Independent contract and synthetic-recipe sanity checks, not model metrics."""

import csv
import re
from dataclasses import replace

import numpy as np
import pandas as pd
import pytest

from ml.load import load_events, load_raw
from ml.split import chronological_split, normal_only
from ml.synth import SynthConfig, generate, write_csv

# Hardcoded independently from the implementation, exactly CLAUDE.md §7.
EXPECTED = [
    "timestamp_iso",
    "pm1_0",
    "pm2_5",
    "pm10",
    "temp_c",
    "humidity_pct",
    "tvoc_ppb",
    "eco2_ppm",
    "co_ppm",
    "label",
]
NUMERIC = EXPECTED[1:-1]
INTS = ["pm1_0", "pm2_5", "pm10", "tvoc_ppb", "eco2_ppm"]
DECIMALS = ["temp_c", "humidity_pct", "co_ppm"]


def indexed(raw):
    return raw.set_index(pd.DatetimeIndex(raw.timestamp_iso)).drop(
        columns="timestamp_iso"
    )


def event_rows(raw, events, label):
    """Select anomaly kinds only via the independently annotated event intervals."""
    times = pd.DatetimeIndex(raw.timestamp_iso)
    mask = np.zeros(len(raw), dtype=bool)
    for event in events.loc[events.label == label].itertuples(index=False):
        mask |= (times >= pd.Timestamp(event.start_iso)) & (
            times < pd.Timestamp(event.end_iso)
        )
    return raw.loc[mask, NUMERIC]


def test_schema_frequency_labels_and_events(week):
    raw, events = week
    assert list(raw.columns) == EXPECTED
    assert list(events.columns) == ["start_iso", "end_iso", "label", "note"]
    assert len(raw) == 7 * 86400
    times = pd.DatetimeIndex(raw.timestamp_iso)
    assert (times[1:] - times[:-1] == pd.Timedelta(seconds=1)).all()
    assert raw.timestamp_iso.str.endswith("+07:00").all()
    assert set(raw.label) == {"normal", "marked"}
    assert events.label.value_counts().to_dict() == {
        "decorrelation": 7,
        "spike": 7,
        "drift": 7,
    }
    covered = np.zeros(len(raw), dtype=int)
    for event in events.itertuples(index=False):
        covered += (times >= pd.Timestamp(event.start_iso)) & (
            times < pd.Timestamp(event.end_iso)
        )
    assert covered.max() == 1
    np.testing.assert_array_equal(covered > 0, raw.label == "marked")


def test_csv_formats_and_missing_roundtrip(output_dir):
    raw, events = generate(1, 42, cfg=SynthConfig(missing_rate=0.05))
    raw_path, event_path = write_csv(raw, events, output_dir)
    with raw_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        assert reader.fieldnames == EXPECTED
        blanks = 0
        for row in reader:
            for column in INTS:
                assert not row[column] or re.fullmatch(r"\d+", row[column])
            for column in DECIMALS:
                assert not row[column] or re.fullmatch(r"\d+\.\d", row[column])
            blanks += sum(row[column] == "" for column in NUMERIC)
    loaded = load_raw(raw_path)
    assert blanks > 0
    assert blanks == loaded[NUMERIC].isna().sum().sum()
    np.testing.assert_allclose(loaded[NUMERIC], raw[NUMERIC], equal_nan=True)
    loaded_events = load_events(event_path)
    assert loaded_events.label.tolist() == events.label.tolist()


def test_reproducibility_byte_identical_and_different_seed(output_dir):
    pairs = []
    for name, seed in [("a", 42), ("b", 42), ("c", 43)]:
        raw, events = generate(1, seed)
        paths = write_csv(raw, events, output_dir / name)
        pairs.append(tuple(path.read_bytes() for path in paths))
    assert pairs[0] == pairs[1]
    assert pairs[0][0] != pairs[2][0]


def test_all_missing_is_blank(output_dir):
    raw, events = generate(10 / 86400, 42, cfg=SynthConfig(missing_rate=1))
    path, _ = write_csv(raw, events, output_dir)
    loaded = load_raw(path)
    assert loaded[NUMERIC].isna().all().all()
    assert ",,,,,,,,,normal" in path.read_text()


def test_normal_daily_anticorrelation_and_th_hold():
    cfg = SynthConfig(missing_rate=0)
    raw, _ = generate(1, 42, cfg=cfg)
    before_cooking = raw.iloc[: cfg.cooking_offsets_s[0]]
    assert before_cooking.temp_c.corr(before_cooking.humidity_pct) < -0.9
    for column in ["temp_c", "humidity_pct"]:
        np.testing.assert_array_equal(raw[column].iloc[::2], raw[column].iloc[1::2])
    assert raw.tvoc_ppb.corr(raw.eco2_ppm) > 0.99
    residual = raw.eco2_ppm - (cfg.eco2_intercept + cfg.eco2_tvoc_gain * raw.tvoc_ppb)
    assert residual.std() < 3


def test_cooking_pm_temp_rh_rise_and_decorrelation_holds_th():
    cfg = SynthConfig(
        missing_rate=0, pm_noise_std=0, temp_noise_std=0, humidity_noise_std=0
    )
    raw, events = generate(1, 42, cfg=cfg)
    # Noon to half an hour: daily T falls and daily RH rises. The cooking
    # pulse must still raise both relative to their original daily baseline.
    start = cfg.cooking_offsets_s[0]
    peak = start + cfg.cooking_duration_s // 2
    assert (raw.loc[start:peak, "label"] == "normal").all()
    phase = 2 * np.pi * peak / 86400
    assert (
        raw.loc[peak, "temp_c"]
        > cfg.temp_base + cfg.temp_daily_amplitude * np.sin(phase) + 1
    )
    assert (
        raw.loc[peak, "humidity_pct"]
        > cfg.humidity_base - cfg.humidity_daily_amplitude * np.sin(phase) + 2
    )
    decor = event_rows(raw, events, "decorrelation")
    assert decor.temp_c.nunique() == decor.humidity_pct.nunique() == 1
    assert (
        decor.pm2_5.max() == raw.loc[peak, "pm2_5"] == cfg.pm_base + cfg.pm_event_rise
    )


@pytest.mark.parametrize("seed", [0, 42, 73])
def test_decorrelation_within_train_normal_three_sigma(seed):
    raw, events = generate(7, seed)
    frame = indexed(raw)
    train, _, _ = chronological_split(
        frame,
        frame.index[4 * 86400],
        frame.index[5 * 86400],
        gap_s=60,
    )
    normal = normal_only(train)[NUMERIC]
    decor = event_rows(raw, events, "decorrelation")
    inside = (decor >= normal.mean() - 3 * normal.std(ddof=0)) & (
        decor <= normal.mean() + 3 * normal.std(ddof=0)
    )
    # Missing points count as outside: denominator includes every event point.
    fraction = inside.all(axis=1).mean()
    print(f"decorrelation seed={seed}: all-channel inside train mean±3σ={fraction:.6%}")
    assert fraction >= 0.95


def test_spike_sanity_and_drift_ramp(week):
    raw, events = week
    frame = indexed(raw)
    train, _, _ = chronological_split(
        frame, frame.index[4 * 86400], frame.index[5 * 86400], 60
    )
    normal = normal_only(train)[NUMERIC]
    spike = event_rows(raw, events, "spike")
    outside = (spike - normal.mean()).abs() > 3 * normal.std(ddof=0)
    fraction = outside.any(axis=1).mean()
    print(f"spike: outside train mean±3σ in any channel={fraction:.6%}")
    assert fraction > 0.95
    for event in events.loc[events.label == "drift"].itertuples(index=False):
        times = pd.DatetimeIndex(raw.timestamp_iso)
        drift = raw.loc[
            (times >= pd.Timestamp(event.start_iso))
            & (times < pd.Timestamp(event.end_iso))
        ]
        assert drift.tvoc_ppb.iloc[-60:].mean() - drift.tvoc_ppb.iloc[:60].mean() > 100


def test_config_controls_missing_and_anomaly_size():
    cfg = SynthConfig(missing_rate=0)
    raw, _ = generate(1, 42, cfg=cfg)
    altered, _ = generate(
        1, 42, cfg=replace(cfg, spike_pm_rise=200, drift_tvoc_rise=300)
    )
    assert not raw[NUMERIC].isna().any().any()
    start = cfg.spike_offset_s
    assert (
        altered.pm2_5.iloc[start : start + cfg.spike_duration_s]
        - raw.pm2_5.iloc[start : start + cfg.spike_duration_s]
        == 100
    ).all()
    assert (
        altered.tvoc_ppb.iloc[cfg.drift_offset_s + cfg.drift_duration_s - 1]
        > raw.tvoc_ppb.iloc[cfg.drift_offset_s + cfg.drift_duration_s - 1] + 100
    )


@pytest.mark.parametrize(
    "cfg",
    [
        SynthConfig(missing_rate=-0.1),
        SynthConfig(missing_rate=1.1),
        SynthConfig(th_hold_s=0),
        SynthConfig(spike_duration_s=0),
        SynthConfig(drift_offset_s=72000),
        SynthConfig(pm_noise_std=float("nan")),
    ],
)
def test_invalid_config(cfg):
    with pytest.raises(ValueError):
        generate(1, 42, cfg=cfg)


@pytest.mark.parametrize(
    "start",
    ["2026-09-23T00:00:00", "2026-09-23T00:00:00Z", "2026-09-23T00:00:00.1+07:00"],
)
def test_invalid_start(start):
    with pytest.raises(ValueError):
        generate(1, 42, start=start)


def test_partial_day_event_is_clipped():
    raw, events = generate((50400 + 100) / 86400, 42)
    assert len(events) == 1
    event = events.iloc[0]
    assert pd.Timestamp(event.end_iso) == pd.Timestamp(
        raw.timestamp_iso.iloc[-1]
    ) + pd.Timedelta(seconds=1)
    assert (raw.label == "marked").sum() == 100


def test_output_rejects_outside_processed(tmp_path):
    raw, events = generate(10 / 86400, 42)
    with pytest.raises(ValueError, match="data/processed"):
        write_csv(raw, events, tmp_path)
