"""Load the unchanged CLAUDE.md §7 raw contract and separate event annotations."""

import csv
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import pandas as pd

RAW_COLUMNS = (
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
)
NUMERIC_COLUMNS = RAW_COLUMNS[1:-1]
INTEGER_COLUMNS = ("pm1_0", "pm2_5", "pm10", "tvoc_ppb", "eco2_ppm")
EVENT_COLUMNS = ("start_iso", "end_iso", "label", "note")


def _read_contract(path, columns):
    with Path(path).open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader, [])
        if header != list(columns):
            raise ValueError(
                f"{path}: header mismatch; expected {list(columns)}, got {header}"
            )
        rows = []
        for line, row in enumerate(reader, 2):
            if len(row) != len(columns):
                raise ValueError(f"{path}:{line}: expected {len(columns)} columns")
            rows.append(row)
    return pd.DataFrame(rows, columns=columns)


def _parse_times(values, column):
    parsed = []
    for value in values:
        try:
            time = datetime.fromisoformat(value)
            if (
                "T" not in value
                or not value.endswith("+07:00")
                or time.utcoffset() != timedelta(hours=7)
            ):
                raise ValueError("expected ISO 8601 with +07:00")
            parsed.append(time)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{column}: invalid +07:00 timestamp {value!r}") from exc
    return pd.DatetimeIndex(parsed, tz="UTC+07:00")


def load_raw(paths):
    """Sort one or several raw CSVs; reject header, timezone and duplicate errors.

    Only empty numeric fields become NaN. Labels stay normal/marked; event
    names must be read from load_events rather than inferred from raw labels.
    """
    if isinstance(paths, (str, Path)):
        paths = [paths]
    frames = [_read_contract(path, RAW_COLUMNS) for path in paths]
    if not frames:
        raise ValueError("load_raw requires at least one path")
    frame = pd.concat(frames, ignore_index=True)
    frame.index = _parse_times(frame.pop("timestamp_iso"), "timestamp_iso")
    frame.index.name = "timestamp_iso"
    if frame.index.has_duplicates:
        duplicates = frame.index[frame.index.duplicated()].unique()
        raise ValueError(f"duplicate timestamp_iso: {duplicates[:3].tolist()}")
    if not frame["label"].isin(["normal", "marked"]).all():
        raise ValueError("label must be normal or marked")
    for column in NUMERIC_COLUMNS:
        text = frame[column]
        try:
            numeric = pd.to_numeric(text.mask(text == ""), errors="raise").astype(float)
        except ValueError as exc:
            raise ValueError(f"{column}: invalid numeric field") from exc
        if not np.isfinite(numeric[text != ""]).all():
            raise ValueError(
                f"{column}: non-finite numeric field; missing must be empty"
            )
        if column in INTEGER_COLUMNS and (numeric.dropna() % 1 != 0).any():
            raise ValueError(f"{column}: expected integer values")
        frame[column] = numeric
    return frame.sort_index()


def load_events(path):
    """Load event intervals [start_iso, end_iso), including their names and notes."""
    frame = _read_contract(path, EVENT_COLUMNS)
    for column in EVENT_COLUMNS[:2]:
        frame[column] = _parse_times(frame[column], column)
    if (frame["end_iso"] <= frame["start_iso"]).any():
        raise ValueError("events require start_iso < end_iso")
    if (frame["label"] == "").any():
        raise ValueError("events require a nonempty label")
    return frame.sort_values("start_iso").reset_index(drop=True)
