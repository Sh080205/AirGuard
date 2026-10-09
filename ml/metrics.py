"""ML-2 evaluation; see METRICS.md for the versioned, onset-based definitions.

No model fitting, threshold selection, interpolation or point adjustment.
"""

import json
from pathlib import Path

import numpy as np
import pandas as pd

DEFINITION_VERSION = "ml2-v1"
DAY_S = 86400


def _times(values, name):
    try:
        result = pd.DatetimeIndex(values)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name}: invalid timestamps") from exc
    if result.tz is None or result.hasnans:
        raise ValueError(f"{name}: timezone-aware, nonmissing timestamps required")
    return result


def _index(index, name):
    if not isinstance(index, pd.DatetimeIndex):
        raise TypeError(f"{name}: DatetimeIndex required")
    _times(index, name)
    if index.has_duplicates or not index.is_monotonic_increasing:
        raise ValueError(f"{name}: sorted unique timestamps required")


def _aligned(series, index, name):
    if not isinstance(series, pd.Series):
        raise TypeError(f"{name}: indexed Series required")
    _index(series.index, name)
    if not series.index.equals(index):
        raise ValueError(f"{name}: timestamp mismatch")


def _events(events, mapping, left, right):
    required = {"start_iso", "end_iso", "label", "note"}
    if not isinstance(events, pd.DataFrame) or not required.issubset(events.columns):
        raise ValueError("events: missing start_iso/end_iso/label/note")
    if not isinstance(mapping, dict) or any(
        not isinstance(key, str)
        or not key.strip()
        or key == "marked"
        or value not in ("normal", "anomaly", "unknown")
        for key, value in mapping.items()
    ):
        raise ValueError(
            "event_mapping: explicit named kinds required; marked is unknown"
        )
    result = events.copy().reset_index(drop=True)
    if result.empty:
        result["kind"] = pd.Series(dtype=str)
        return result
    starts = _times(result.start_iso, "event start")
    ends = _times(result.end_iso, "event end")
    if (ends <= starts).any():
        raise ValueError("events: require start < end")
    if (starts < left).any() or (ends > right).any():
        raise ValueError(
            "events: outside observation bounds; select this split explicitly"
        )
    if not starts.is_monotonic_increasing:
        raise ValueError("events: sort by start_iso first")
    if len(starts) > 1 and (starts[1:] < ends[:-1]).any():
        raise ValueError("events: overlapping annotations are ambiguous")
    if not result.label.map(lambda x: isinstance(x, str)).all():
        raise ValueError("events: label must be a string (empty means unknown)")
    if not result.note.map(lambda x: isinstance(x, str)).all():
        raise ValueError("events: note must be a string (empty is allowed)")
    result["start_iso"], result["end_iso"] = starts, ends
    result["kind"] = result.label.map(mapping).fillna("unknown")
    return result


def _average_precision(truth, scores):
    """Unweighted noninterpolated AP, grouping equal scores at one threshold."""
    positives = int(truth.sum())
    if not positives:
        return None
    order = np.argsort(-scores, kind="stable")
    ranked = truth[order]
    # Evaluate after *all* rows at a tied score, independent of tie ordering.
    sorted_scores = scores[order]
    ends = np.r_[
        np.flatnonzero(sorted_scores[1:] != sorted_scores[:-1]), len(scores) - 1
    ]
    tp = np.cumsum(ranked)[ends]
    precision = tp / (ends + 1)
    return float(np.sum(np.diff(np.r_[0, tp]) / positives * precision))


def evaluate(
    raw, scores, events, *, observed_until, event_mapping, threshold, threshold_source
):
    """Return (summary dict, audit DataFrame) without changing any input.

    raw is load_raw's indexed frame. scores and observed_until are exactly
    aligned Series. Each row has explicit nonoverlapping observed support
    [timestamp, observed_until); a missing score breaks an alarm episode and
    is excluded from point metrics/normal exposure. Larger score = anomalous.
    threshold_source is fixed/train/validation; select and freeze before test.
    """
    if not isinstance(raw, pd.DataFrame) or raw.empty:
        raise ValueError("raw: nonempty DataFrame required")
    _index(raw.index, "raw")
    if "label" not in raw or not raw.label.isin(["normal", "marked"]).all():
        raise ValueError("raw: label must be normal/marked")
    _aligned(scores, raw.index, "scores")
    _aligned(observed_until, raw.index, "observed_until")
    ends = _times(observed_until, "observed_until")
    times = raw.index
    if (ends <= times).any() or (len(times) > 1 and (ends[:-1] > times[1:]).any()):
        raise ValueError("observed_until: require positive, nonoverlapping support")
    try:
        values = scores.to_numpy(dtype=float, na_value=np.nan)
        threshold = float(threshold)
    except (TypeError, ValueError) as exc:
        raise ValueError("scores/threshold: numeric values required") from exc
    if np.isinf(values).any() or not np.isfinite(threshold):
        raise ValueError("scores/threshold: finite values or missing scores required")
    if threshold_source not in ("fixed", "train", "validation"):
        raise ValueError("threshold_source: fixed/train/validation only; never test")
    annotations = _events(events, event_mapping, times[0], ends[-1])
    truth = np.where(raw.label == "normal", "normal", "unknown").astype(object)
    event_id = np.full(len(times), -1, dtype=int)
    for number, event in annotations.iterrows():
        mask = (times >= event.start_iso) & (times < event.end_iso)
        truth[mask], event_id[mask] = event.kind, number
    available = ~np.isnan(values)
    alarm = available & (values >= threshold)
    episodes = []
    for i in np.flatnonzero(alarm):
        if i > 0 and alarm[i - 1] and times[i] == ends[i - 1]:
            episodes[-1]["end_iso"] = ends[i].isoformat()
        else:
            episodes.append(
                {
                    "start_iso": times[i].isoformat(),
                    "end_iso": ends[i].isoformat(),
                    "onset_truth": truth[i],
                    "onset_event_id": int(event_id[i]),
                }
            )
    detected, delays, event_results = 0, [], []
    for number, event in annotations.iterrows():
        if event.kind != "anomaly":
            continue
        onsets = [
            pd.Timestamp(ep["start_iso"])
            for ep in episodes
            if event.start_iso <= pd.Timestamp(ep["start_iso"]) < event.end_iso
        ]
        early = sum(
            pd.Timestamp(ep["start_iso"])
            < event.start_iso
            < pd.Timestamp(ep["end_iso"])
            for ep in episodes
        )
        delay = (min(onsets) - event.start_iso).total_seconds() if onsets else None
        detected += bool(onsets)
        if delay is not None:
            delays.append(delay)
        mask = (times >= event.start_iso) & (times < event.end_iso)
        event_results.append(
            {
                "event_id": int(number),
                "label": event.label,
                "note": event.note,
                "start_iso": event.start_iso.isoformat(),
                "end_iso": event.end_iso.isoformat(),
                "detected": bool(onsets),
                "delay_s": delay,
                "alarm_onsets": len(onsets),
                "early_overlapping_episodes": int(early),
                "observed_points": int(mask.sum()),
                "scored_points": int((mask & available).sum()),
            }
        )
    known = (truth != "unknown") & available
    positive = truth[known] == "anomaly"
    predicted = alarm[known]
    tp = int((positive & predicted).sum())
    fp = int((~positive & predicted).sum())
    fn = int((positive & ~predicted).sum())
    # Integrate annotated truth over support, not over timestamp differences.
    # Partial row overlaps count exactly; MARK gaps never become normal time.
    durations = (ends - times).total_seconds().to_numpy()
    base_normal = (raw.label == "normal").to_numpy()
    normal_durations = durations * base_normal
    for event in annotations.itertuples(index=False):
        left = times.where(times >= event.start_iso, event.start_iso)
        right = ends.where(ends <= event.end_iso, event.end_iso)
        overlap = np.maximum(0, (right - left).total_seconds().to_numpy())
        normal_durations += overlap * (
            (event.kind == "normal") - base_normal.astype(int)
        )
    normal_s = float(normal_durations[available].sum())
    false_episodes = sum(ep["onset_truth"] == "normal" for ep in episodes)
    event_count = len(event_results)
    summary = {
        "definition_version": DEFINITION_VERSION,
        "pr_auc_definition": "noninterpolated unweighted average precision (AP)",
        "threshold": threshold,
        "threshold_source": threshold_source,
        "event_mapping": dict(event_mapping),
        "points": len(raw),
        "evaluated_points": int(known.sum()),
        "unknown_points": int((truth == "unknown").sum()),
        "missing_score_points": int((~available).sum()),
        "observed_s": float(durations.sum()),
        "scored_observed_s": float(durations[available].sum()),
        "positive_points": int(positive.sum()),
        "ap": _average_precision(positive, values[known]),
        "tp": tp,
        "fp": fp,
        "fn": fn,
        "precision": tp / (tp + fp) if tp + fp else None,
        "recall": tp / (tp + fn) if tp + fn else None,
        "event_count": event_count,
        "detected_events": detected,
        "missed_events": event_count - detected,
        "event_detection_rate": detected / event_count if event_count else None,
        "mttd_detected_s": float(np.mean(delays)) if delays else None,
        "normal_observed_s": normal_s,
        "normal_observed_days": normal_s / DAY_S,
        "false_alarm_episodes": false_episodes,
        "false_alarm_episodes_per_day": false_episodes * DAY_S / normal_s
        if normal_s
        else None,
        "unscored_alarm_episodes": sum(
            ep["onset_truth"] == "unknown" for ep in episodes
        ),
        "events": event_results,
        "alarm_episodes": episodes,
        "unknown_annotations": int((annotations.kind == "unknown").sum()),
    }
    audit = pd.DataFrame(
        {
            "raw_label": raw.label,
            "truth": truth,
            "event_id": event_id,
            "score": values,
            "alarm": alarm,
            "evaluated": known,
            "observed_until": ends,
        },
        index=times,
    )
    return summary, audit


def save_result(out, summary, audit):
    """Write JSON (undefined = null) and unchanged point decisions for review."""
    out = Path(out).resolve()
    allowed = Path(__file__).resolve().parents[1] / "data" / "processed"
    if not out.is_relative_to(allowed.resolve()):
        raise ValueError("metrics output must be under data/processed/")
    payload = json.dumps(summary, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    out.mkdir(parents=True, exist_ok=True)
    path = out / "metrics.json"
    path.write_text(payload, encoding="utf-8")
    audit.to_csv(out / "points.csv", index_label="timestamp_iso", lineterminator="\n")
    return path
