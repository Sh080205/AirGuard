"""Time splits and frozen, train-normal-only normalization (no model metrics)."""

import json
from pathlib import Path

import numpy as np
import pandas as pd

try:  # Both `python ml/synth.py` style imports and `import ml.split` work.
    from .load import NUMERIC_COLUMNS
except ImportError:
    from load import NUMERIC_COLUMNS


def chronological_split(df, t1, t2, gap_s=0):
    """Return train < t1, val in [t1+gap,t2), test >= t2+gap.

    The excluded intervals [t1,t1+gap) and [t2,t2+gap) prevent windows
    spanning a boundary. No rows are shuffled, relabeled or interpolated.
    """
    if not isinstance(df.index, pd.DatetimeIndex) or df.index.tz is None:
        raise ValueError("split requires a timezone-aware DatetimeIndex")
    if not df.index.is_monotonic_increasing or df.index.has_duplicates:
        raise ValueError("split requires sorted, unique timestamps")
    t1, t2 = pd.Timestamp(t1), pd.Timestamp(t2)
    if pd.isna(t1) or pd.isna(t2) or t1.tzinfo is None or t2.tzinfo is None:
        raise ValueError("split boundaries require timezone-aware timestamps")
    if not np.isfinite(gap_s) or gap_s < 0:
        raise ValueError("gap_s must be finite and nonnegative")
    gap = pd.Timedelta(seconds=gap_s)
    if t1 + gap >= t2:
        raise ValueError("require t1 + gap < t2")
    train = df.loc[df.index < t1].copy()
    val = df.loc[(df.index >= t1 + gap) & (df.index < t2)].copy()
    test = df.loc[df.index >= t2 + gap].copy()
    return train, val, test


def normal_only(df):
    """Cooking remains normal; MARK intervals are excluded from fitting."""
    return df.loc[df["label"] == "normal"].copy()


def fit_normalizer(train, columns=NUMERIC_COLUMNS):
    """Fit each numeric channel on train normal rows, skipping missing values.

    Population std uses sqrt(mean((x-mean)**2)), ddof=0. A constant
    channel uses scale 1 so it stays finite; an unobserved channel fails.
    Call this only with the train returned by chronological_split.
    columns selects a §7-ordered subset (e.g. PM+T/RH before ENS160/CO exist);
    the default keeps every numeric channel as in ML-1.
    """
    columns = _check_columns(columns)
    normal = normal_only(train).loc[:, columns]
    if (normal.count() < 2).any():
        raise ValueError("normalizer requires at least two normal values per channel")
    if np.isinf(normal.to_numpy(dtype=float)).any():
        raise ValueError("normalizer does not accept infinite values")
    mean = normal.mean()
    std = normal.std(ddof=0)
    # Floating subtraction can produce a tiny nonzero std for constant 0.4.
    std = std.mask(normal.nunique(dropna=True) == 1, 1.0)
    params = {
        "columns": columns,
        "mean": mean.to_dict(),
        "std": std.to_dict(),
        "ddof": 0,
    }
    _validate_params(params)
    return params


def _check_columns(columns):
    """Return a nonempty, duplicate-free list of numeric columns in §7 order."""
    columns = list(columns)
    order = [column for column in NUMERIC_COLUMNS if column in columns]
    if not columns or columns != order or len(set(columns)) != len(columns):
        raise ValueError("normalizer columns must be a §7-ordered numeric subset")
    return columns


def _validate_params(params):
    columns = params.get("columns")
    if not isinstance(columns, list) or params.get("ddof") != 0:
        raise ValueError("normalizer columns/ddof mismatch")
    _check_columns(columns)
    for key in ("mean", "std"):
        if set(params.get(key, {})) != set(columns):
            raise ValueError(f"normalizer {key} columns mismatch")
        values = np.array([params[key][column] for column in columns], dtype=float)
        if not np.isfinite(values).all() or (key == "std" and (values <= 0).any()):
            raise ValueError(f"normalizer {key} must be finite; std must be positive")


def apply_normalizer(df, params):
    """Apply saved (x-mean)/std without fitting; preserve NaN, time and labels."""
    _validate_params(params)
    result = df.copy()
    for column in params["columns"]:
        result[column] = (df[column] - params["mean"][column]) / params["std"][column]
    return result


def save_normalizer(path, params):
    """Persist explicit fitted parameters; pass a norm.json path or directory."""
    _validate_params(params)
    path = Path(path)
    if path.suffix != ".json":
        path = path / "norm.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(params, indent=2, allow_nan=False) + "\n", encoding="utf-8"
    )
    return path
