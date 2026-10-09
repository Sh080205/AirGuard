"""Hand-computed counterexamples; no trained model or adjusted predictions."""

import json

import numpy as np
import pandas as pd
import pytest

from ml.load import load_events, load_raw
from ml.metrics import evaluate, save_result

START = pd.Timestamp("2026-09-23T00:00:00+07:00")
MAPPING = {"spike": "anomaly", "drift": "anomaly", "cooking": "normal"}


def stamp(seconds):
    return START + pd.Timedelta(seconds=seconds)


def annotations(*rows):
    return pd.DataFrame(
        [(stamp(a), stamp(b), label, note) for a, b, label, note in rows],
        columns=["start_iso", "end_iso", "label", "note"],
    )


def inputs(values, labels=None, times=None, durations=None):
    times = list(range(len(values))) if times is None else times
    index = pd.DatetimeIndex([stamp(t) for t in times])
    raw = pd.DataFrame({"label": labels or ["normal"] * len(values)}, index=index)
    scores = pd.Series(values, index=index, dtype=float)
    durations = [1] * len(values) if durations is None else durations
    until = pd.Series(
        [stamp(t + d) for t, d in zip(times, durations, strict=True)], index=index
    )
    return raw, scores, until


def run(values, events=None, **kwargs):
    raw, scores, until = inputs(values, **kwargs)
    return evaluate(
        raw,
        scores,
        annotations() if events is None else events,
        observed_until=until,
        event_mapping=MAPPING,
        threshold=0.5,
        threshold_source="fixed",
    )


def test_hand_ap_precision_recall_and_missed_event():
    # Ranked truth: 1,0,1,0 => AP=(1 + 2/3)/2 = 5/6.
    # t0 detected immediately, t2 missed; no point adjustment of t2.
    events = annotations((0, 1, "spike", ""), (2, 3, "drift", ""))
    result, points = run([0.9, 0.8, 0.4, 0.1], events)
    assert result["ap"] == pytest.approx(5 / 6)
    assert (result["tp"], result["fp"], result["fn"]) == (1, 1, 1)
    assert result["precision"] == result["recall"] == 0.5
    assert result["event_detection_rate"] == 0.5
    assert result["missed_events"] == 1
    assert result["mttd_detected_s"] == 0
    assert result["events"][1]["delay_s"] is None
    assert points.alarm.tolist() == [True, True, False, False]


def test_tied_ap_is_one_threshold_independent_of_order():
    # Both tied positives/negatives enter together: precision=1/2, delta R=1.
    for at in [0, 1]:
        result, _ = run([0.7, 0.7], annotations((at, at + 1, "spike", "")))
        assert result["ap"] == 0.5


def test_all_positive_has_ap_one_and_no_normal_rate():
    result, _ = run([0.9, 0.1], annotations((0, 2, "spike", "")))
    assert result["ap"] == 1
    assert result["precision"] == 1
    assert result["recall"] == 0.5
    assert result["normal_observed_s"] == 0
    assert result["false_alarm_episodes_per_day"] is None


def test_all_normal_no_positive():
    result, _ = run([0.9, 0.8, 0.1, 0.9])
    assert result["ap"] is result["recall"] is None
    assert result["precision"] == 0
    assert result["event_detection_rate"] is result["mttd_detected_s"] is None
    assert result["event_count"] == result["missed_events"] == 0
    assert result["false_alarm_episodes"] == 2
    assert result["normal_observed_s"] == 4
    assert result["false_alarm_episodes_per_day"] == 43200


def test_no_alarms_and_all_missed():
    result, _ = run([0, 0, 0], annotations((1, 3, "spike", "")))
    assert result["precision"] is result["mttd_detected_s"] is None
    assert result["recall"] == result["event_detection_rate"] == 0
    assert result["missed_events"] == 1


def test_multiple_alarms_event_and_unchanged_predictions():
    result, points = run([0, 0, 0.6, 0.7, 0, 0.9, 0], annotations((1, 6, "spike", "")))
    assert result["detected_events"] == 1
    assert result["events"][0]["alarm_onsets"] == 2
    assert result["mttd_detected_s"] == 1
    assert result["recall"] == 3 / 5
    assert points.alarm.tolist() == [False, False, True, True, False, True, False]


@pytest.mark.parametrize(
    "values,detected,false_count,delay",
    [
        ([0, 0.5, 0, 0], 1, 0, 0),  # start inclusive, score == threshold inclusive
        ([0, 0, 0, 0.5], 0, 1, None),  # end exclusive
        ([0.5, 0.5, 0.5, 0], 0, 1, None),  # early ongoing alarm
        ([0.5, 0, 0.5, 0], 1, 1, 1),  # early ends; fresh in-event onset
    ],
)
def test_boundaries_and_early_alarm(values, detected, false_count, delay):
    result, _ = run(values, annotations((1, 3, "spike", "")))
    assert result["detected_events"] == detected
    assert result["missed_events"] == 1 - detected
    assert result["false_alarm_episodes"] == false_count
    assert result["mttd_detected_s"] == delay


def test_early_overlap_is_explicit_and_not_negative_latency():
    result, _ = run([1, 1, 1, 0], annotations((1, 3, "spike", "")))
    assert result["events"][0]["early_overlapping_episodes"] == 1
    assert result["event_detection_rate"] == 0
    assert result["mttd_detected_s"] is None


def test_cooking_marked_unknown_and_notes_do_not_create_truth():
    events = annotations(
        (0, 2, "cooking", ""),
        (2, 3, "marked", "spike"),
        (3, 4, "incense", ""),
        (4, 5, "", "drift"),
    )
    result, points = run([1] * 6, events, labels=["marked"] * 6)
    assert points.truth.tolist() == [
        "normal",
        "normal",
        "unknown",
        "unknown",
        "unknown",
        "unknown",
    ]
    assert result["event_count"] == 0
    assert result["unknown_annotations"] == 3
    assert result["normal_observed_s"] == 2
    assert result["evaluated_points"] == 2
    # Truth transitions do not split or regenerate predictions/episodes.
    assert len(result["alarm_episodes"]) == 1


def test_explicit_mapping_overrides_raw_normal_too():
    result, points = run([0, 1, 0], annotations((1, 2, "spike", "")))
    assert points.truth.tolist() == ["normal", "anomaly", "normal"]
    assert result["recall"] == 1


def test_actual_exposure_gaps_and_missing_scores_break_episodes():
    result, _ = run([1, 1, np.nan, 1], times=[0, 1, 10, 20])
    assert result["normal_observed_s"] == 3  # not 21 seconds of wall time
    assert result["observed_s"] == 4
    assert result["scored_observed_s"] == 3
    assert result["false_alarm_episodes"] == 2
    assert result["false_alarm_episodes_per_day"] == 57600
    assert result["missing_score_points"] == 1
    contiguous, _ = run([1, np.nan, 1])
    assert contiguous["false_alarm_episodes"] == 2
    assert contiguous["normal_observed_s"] == 2


def test_duration_with_downtime_and_partial_final_sample():
    # Times span more than a day, but only two hours were actually observed.
    result, _ = run([1, 0, 1], times=[0, 3600, 86400], durations=[1800, 1800, 3600])
    assert result["normal_observed_s"] == 7200
    assert result["normal_observed_days"] == pytest.approx(1 / 12)
    assert result["false_alarm_episodes"] == 2
    assert result["false_alarm_episodes_per_day"] == 24
    # Last sample observed for only 0.25 s; never assume an extra whole second.
    result, _ = run([1, 1], durations=[1, 0.25])
    assert result["normal_observed_s"] == 1.25
    assert result["false_alarm_episodes_per_day"] == 69120


def test_touching_events_do_not_split_continuous_alarm():
    events = annotations((0, 1, "spike", ""), (1, 3, "drift", ""))
    result, _ = run([1, 1, 1], events)
    assert len(result["alarm_episodes"]) == 1
    assert result["detected_events"] == result["missed_events"] == 1
    assert result["events"][1]["early_overlapping_episodes"] == 1
    # If the alarm first starts exactly at the shared boundary, only event 2 detects.
    result, _ = run([0, 1, 1], events)
    assert [e["detected"] for e in result["events"]] == [False, True]


def test_partial_support_intersection_uses_real_normal_duration():
    # [0,10) observed; typed anomaly [2,4) => exactly 8 seconds normal.
    result, _ = run([1], annotations((2, 4, "spike", "")), durations=[10])
    assert result["normal_observed_s"] == 8
    assert result["false_alarm_episodes_per_day"] == 10800
    assert result["missed_events"] == 1  # no sample/onset inside interval
    assert result["events"][0]["scored_points"] == 0
    # Explicit cooking restores only its interval within a MARK support.
    result, _ = run(
        [0], annotations((2, 4, "cooking", "")), durations=[10], labels=["marked"]
    )
    assert result["normal_observed_s"] == 2


def test_event_in_gap_and_missing_event_scores_still_missed():
    result, _ = run([0, 0], annotations((2, 4, "spike", "")), times=[0, 10])
    assert result["event_count"] == result["missed_events"] == 1
    assert result["events"][0]["observed_points"] == 0
    result, _ = run([0, np.nan, np.nan], annotations((1, 3, "spike", "")))
    assert result["missed_events"] == 1
    assert result["event_detection_rate"] == 0
    assert result["mttd_detected_s"] is None


def test_all_missing_or_unknown_undefined_values():
    for kwargs in [{}, {"labels": ["marked", "marked"]}]:
        result, _ = run([np.nan, np.nan], **kwargs)
        assert result["ap"] is result["precision"] is result["recall"] is None
        assert result["false_alarm_episodes_per_day"] is None
        assert result["normal_observed_s"] == 0
    result, _ = run([1, 1], labels=["marked", "marked"])
    assert result["unscored_alarm_episodes"] == 1
    assert result["false_alarm_episodes"] == 0


def test_mean_delay_and_missed_are_reported_together():
    events = annotations((0, 3, "spike", ""), (4, 7, "spike", ""), (8, 10, "spike", ""))
    result, _ = run([0, 1, 0, 0, 0, 0, 1, 0, 0, 0], events)
    assert result["mttd_detected_s"] == 1.5  # delays 1 and 2; third missed
    assert result["event_detection_rate"] == 2 / 3
    assert result["missed_events"] == 1


@pytest.mark.parametrize(
    "rows",
    [
        ((-1, 1, "spike", ""),),
        ((0, 4, "spike", ""),),
        ((1, 1, "spike", ""),),
        ((2, 1, "spike", ""),),
        ((0, 2, "spike", ""), (1, 3, "cooking", "")),
        ((2, 3, "spike", ""), (0, 1, "spike", "")),
    ],
)
def test_invalid_event_intervals(rows):
    with pytest.raises(ValueError):
        run([0, 0, 0], annotations(*rows))


@pytest.mark.parametrize(
    "change",
    [
        "empty",
        "duplicate",
        "unsorted",
        "naive",
        "nat",
        "label",
        "missing-label",
        "scores-mismatch",
        "scores-order",
        "scores-length",
        "scores-inf",
        "scores-text",
        "until-mismatch",
        "until-naive",
        "until-zero",
        "until-overlap",
        "until-missing",
        "threshold-nan",
        "threshold-test",
        "events-columns",
        "event-naive",
        "event-missing",
        "event-label",
        "event-note",
        "mapping-marked",
        "mapping-value",
    ],
)
def test_invalid_missing_and_time_mismatch(change):
    raw, scores, until = inputs([0, 1, 0])
    events = annotations((1, 2, "spike", ""))
    params = {
        "observed_until": until,
        "event_mapping": MAPPING,
        "threshold": 0.5,
        "threshold_source": "fixed",
    }
    if change == "empty":
        raw = raw.iloc[:0]
    elif change == "duplicate":
        raw.index = pd.DatetimeIndex([stamp(0)] * 3)
    elif change == "unsorted":
        raw = raw.iloc[::-1]
    elif change == "naive":
        raw.index = raw.index.tz_localize(None)
    elif change == "nat":
        raw.index = pd.DatetimeIndex([stamp(0), stamp(1), pd.NaT])
    elif change == "label":
        raw.loc[stamp(0), "label"] = "spike"
    elif change == "missing-label":
        raw = raw.drop(columns="label")
    elif change == "scores-mismatch":
        scores.index = scores.index + pd.Timedelta(seconds=1)
    elif change == "scores-order":
        scores = scores.iloc[::-1]
    elif change == "scores-length":
        scores = scores.iloc[:2]
    elif change == "scores-inf":
        scores.iloc[0] = np.inf
    elif change == "scores-text":
        scores = pd.Series(["bad"] * 3, index=raw.index)
    elif change == "until-mismatch":
        until.index = until.index + pd.Timedelta(seconds=1)
    elif change == "until-naive":
        params["observed_until"] = until.dt.tz_localize(None)
    elif change == "until-zero":
        until.iloc[0] = stamp(0)
    elif change == "until-overlap":
        until.iloc[0] = stamp(2)
    elif change == "until-missing":
        until.iloc[0] = pd.NaT
    elif change == "threshold-nan":
        params["threshold"] = np.nan
    elif change == "threshold-test":
        params["threshold_source"] = "test"
    elif change == "events-columns":
        events = events.drop(columns="note")
    elif change == "event-naive":
        events.start_iso = events.start_iso.dt.tz_localize(None)
    elif change == "event-missing":
        events.loc[0, "start_iso"] = pd.NaT
    elif change == "event-label":
        events.loc[0, "label"] = None
    elif change == "event-note":
        events.loc[0, "note"] = None
    elif change == "mapping-marked":
        params["event_mapping"] = {"marked": "anomaly"}
    elif change == "mapping-value":
        params["event_mapping"] = {"spike": "yes"}
    with pytest.raises(ValueError):
        evaluate(raw, scores, events, **params)


def test_csv_join_and_serialization(output_dir, tmp_path):
    raw_path = output_dir / "raw.csv"
    raw_path.write_text(
        "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,tvoc_ppb,eco2_ppm,co_ppm,label\n"
        "2026-09-23T00:00:00+07:00,,,,,,,,,marked\n"
        "2026-09-23T00:00:01+07:00,,,,,,,,,marked\n",
        encoding="utf-8",
    )
    event_path = output_dir / "events.csv"
    event_path.write_text(
        "start_iso,end_iso,label,note\n"
        "2026-09-23T00:00:00+07:00,2026-09-23T00:00:01+07:00,cooking,\n"
        "2026-09-23T00:00:01+07:00,2026-09-23T00:00:02+07:00,marked,\n",
        encoding="utf-8",
    )
    raw, events = load_raw(raw_path), load_events(event_path)
    _, scores, until = inputs([1, 1])
    originals = (
        raw.copy(deep=True),
        scores.copy(deep=True),
        events.copy(deep=True),
        until.copy(deep=True),
    )
    result, points = evaluate(
        raw,
        scores,
        events,
        observed_until=until,
        event_mapping=MAPPING,
        threshold=0.5,
        threshold_source="validation",
    )
    assert points.truth.tolist() == ["normal", "unknown"]
    assert result["positive_points"] == 0
    path = save_result(output_dir / "metrics", result, points)
    assert json.loads(path.read_text(encoding="utf-8"))["ap"] is None
    assert pd.read_csv(path.parent / "points.csv").alarm.tolist() == [True, True]
    for before, after in zip(originals, (raw, scores, events, until), strict=True):
        if isinstance(before, pd.Series):
            pd.testing.assert_series_equal(before, after)
        else:
            pd.testing.assert_frame_equal(before, after)
    with pytest.raises(ValueError, match="data/processed"):
        save_result(tmp_path, result, points)
