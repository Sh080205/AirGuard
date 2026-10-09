import copy
import json

import numpy as np
import pandas as pd
import pytest

from ml.split import (
    apply_normalizer,
    chronological_split,
    fit_normalizer,
    normal_only,
    save_normalizer,
)

NUMERIC = [
    "pm1_0",
    "pm2_5",
    "pm10",
    "temp_c",
    "humidity_pct",
    "tvoc_ppb",
    "eco2_ppm",
    "co_ppm",
]


@pytest.fixture
def frame():
    index = pd.date_range("2026-09-23T00:00:00+07:00", periods=100, freq="s")
    return pd.DataFrame(
        {
            **{column: np.arange(100, dtype=float) for column in NUMERIC},
            "label": "normal",
        },
        index=index,
    )


@pytest.mark.parametrize("gap", [0, 5, 12])
def test_time_split_exact_boundaries_gap_and_no_overlap(frame, gap):
    train, val, test = chronological_split(frame, frame.index[40], frame.index[70], gap)
    pd.testing.assert_frame_equal(train, frame.iloc[:40])
    pd.testing.assert_frame_equal(val, frame.iloc[40 + gap : 70])
    pd.testing.assert_frame_equal(test, frame.iloc[70 + gap :])
    assert train.index.max() < val.index.min() < test.index.min()
    assert val.index.max() < test.index.min()
    assert (val.index.min() - train.index.max()).total_seconds() >= gap
    assert (test.index.min() - val.index.max()).total_seconds() >= gap
    assert len(train) + len(val) + len(test) == len(frame) - 2 * gap
    train.iloc[0, 0] = -999
    assert frame.iloc[0, 0] == 0


def test_split_uses_elapsed_time_for_irregular_data(frame):
    irregular = frame.drop(frame.index[42:47])
    train, val, test = chronological_split(
        irregular, frame.index[40], frame.index[70], 5
    )
    assert val.index.min() == frame.index[47]
    assert train.index.max() == frame.index[39]
    assert test.index.min() == frame.index[75]


@pytest.mark.parametrize("gap", [-1, float("nan"), float("inf"), 30])
def test_bad_gap(frame, gap):
    with pytest.raises(ValueError):
        chronological_split(frame, frame.index[40], frame.index[70], gap)


def test_bad_order_duplicates_and_naive_times(frame):
    for bad in [
        frame.iloc[::-1],
        pd.concat([frame, frame.iloc[:1]]),
        frame.set_axis(frame.index.tz_localize(None)),
        frame.reset_index(drop=True),
    ]:
        with pytest.raises(ValueError):
            chronological_split(bad, frame.index[40], frame.index[70], 5)
    with pytest.raises(ValueError):
        chronological_split(frame, "2026-09-23T00:00:40", frame.index[70], 5)
    with pytest.raises(ValueError):
        chronological_split(frame, frame.index[70], frame.index[40], 5)


def test_train_normal_only_excludes_marked_and_missing(frame):
    train = frame.iloc[:40].copy()
    train.loc[train.index[10:20], "label"] = "marked"
    train.loc[train.label == "marked", NUMERIC] = 1e9
    train.loc[train.index[0], "pm2_5"] = np.nan
    normal = normal_only(train)
    assert len(normal) == 30
    params = fit_normalizer(train)
    for column in NUMERIC:
        assert params["mean"][column] == pytest.approx(normal[column].mean())
        assert params["std"][column] == pytest.approx(normal[column].std(ddof=0))
    transformed = apply_normalizer(normal, params)
    np.testing.assert_allclose(transformed[NUMERIC].mean(), 0, atol=1e-12)
    np.testing.assert_allclose(transformed[NUMERIC].std(ddof=0), 1)
    assert np.isnan(transformed.pm2_5.iloc[0])


def test_no_leakage_and_saved_params_reused(frame, output_dir):
    train, val, test = chronological_split(frame, frame.index[40], frame.index[70], 5)
    params = fit_normalizer(train)
    original = copy.deepcopy(params)
    path = save_normalizer(output_dir, params)
    assert path.name == "norm.json"
    saved = json.loads(path.read_text())
    assert saved == params
    for part in [val, test]:
        before = part.copy()
        scaled = apply_normalizer(part, saved)
        pd.testing.assert_frame_equal(part, before)
        np.testing.assert_allclose(
            scaled.pm2_5, (part.pm2_5 - 19.5) / np.arange(40).std()
        )
        assert scaled.index.equals(part.index)
        assert scaled.label.equals(part.label)
        part.loc[:, NUMERIC] = 1e12
        extreme = apply_normalizer(part, saved)
        assert (extreme[NUMERIC] > 1e9).all().all()
    assert params == saved == original == fit_normalizer(train)


def test_constant_channel_and_insufficient_normal_data(frame):
    frame["co_ppm"] = 0.4
    assert fit_normalizer(frame)["std"]["co_ppm"] == 1
    for bad in [
        frame.iloc[:0],
        frame.iloc[:1],
        frame.assign(label="marked"),
        frame.assign(co_ppm=np.nan),
        frame.assign(co_ppm=np.inf),
    ]:
        with pytest.raises(ValueError):
            fit_normalizer(bad)


def test_invalid_saved_normalizer(frame):
    params = fit_normalizer(frame)
    for key, value in [("std", 0), ("std", -1), ("mean", np.nan)]:
        invalid = copy.deepcopy(params)
        invalid[key]["pm2_5"] = value
        with pytest.raises(ValueError):
            apply_normalizer(frame, invalid)


def test_normalizer_column_subset_for_pm_th(frame):
    subset = ["pm2_5", "temp_c", "humidity_pct"]
    frame[["tvoc_ppb", "eco2_ppm", "co_ppm"]] = np.nan  # ENS160/CO not fitted yet
    params = fit_normalizer(frame, columns=subset)
    assert params["columns"] == subset
    assert set(params["mean"]) == set(subset)
    scaled = apply_normalizer(frame, params)
    np.testing.assert_allclose(scaled[subset].mean(), 0, atol=1e-12)
    assert scaled.tvoc_ppb.isna().all()
    with pytest.raises(ValueError):
        fit_normalizer(frame)  # default still requires every channel


@pytest.mark.parametrize(
    "columns",
    [[], ["temp_c", "pm2_5"], ["pm2_5", "pm2_5"], ["pressure_hpa"], ["label"]],
)
def test_normalizer_rejects_unordered_or_unknown_columns(frame, columns):
    with pytest.raises(ValueError):
        fit_normalizer(frame, columns=columns)
    params = fit_normalizer(frame, columns=["pm2_5"])
    params["columns"] = columns
    with pytest.raises(ValueError):
        apply_normalizer(frame, params)
