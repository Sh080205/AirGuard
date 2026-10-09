"""Deterministic hand example for ML-2; no training or sensor measurements.

Run from the repo root: python -m ml.metrics_example
"""

import json
from pathlib import Path

import pandas as pd

from .metrics import evaluate, save_result


def main():
    times = pd.date_range("2026-09-23T00:00:00+07:00", periods=10, freq="s")
    raw = pd.DataFrame({"label": ["marked"] * 9 + ["normal"]}, index=times)
    # One early cooking alarm continues into the first anomaly. Fresh onsets
    # at t=3 and t=5 detect that event twice. Second anomaly is wholly missed.
    scores = pd.Series([0.9, 0.9, 0.1, 0.9, 0.1, 0.9, 0.1, 0.1, 0.9, 0.9], index=times)
    events = pd.DataFrame(
        [
            (times[0], times[1], "cooking", "explicitly normal"),
            (times[1], times[6], "decorrelation", "typed synthetic anomaly"),
            (times[6], times[8], "spike", "missed synthetic event"),
            (times[8], times[9], "marked", ""),
        ],
        columns=["start_iso", "end_iso", "label", "note"],
    )
    summary, audit = evaluate(
        raw,
        scores,
        events,
        observed_until=pd.Series(times + pd.Timedelta(seconds=1), index=times),
        event_mapping={
            "cooking": "normal",
            "decorrelation": "anomaly",
            "spike": "anomaly",
        },
        threshold=0.5,
        threshold_source="fixed",
    )
    out = Path(__file__).resolve().parents[1] / "data" / "processed" / "metrics-example"
    path = save_result(out, summary, audit)
    keys = [
        "ap",
        "precision",
        "recall",
        "event_detection_rate",
        "missed_events",
        "mttd_detected_s",
        "normal_observed_s",
        "false_alarm_episodes",
        "false_alarm_episodes_per_day",
        "unscored_alarm_episodes",
    ]
    print("Hand example only; not measured model performance or sensor accuracy")
    print(json.dumps({key: summary[key] for key in keys}, indent=2))
    print(f"output={path}")


if __name__ == "__main__":
    main()
