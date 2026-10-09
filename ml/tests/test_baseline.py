"""ML-3 tests: feature sets, train-only fit, conditioning, metrics, C export."""

import json
import math
import re
import shutil
import subprocess

import numpy as np
import pandas as pd
import pytest

from ml import baseline as b
from ml.load import NUMERIC_COLUMNS
from ml.split import apply_normalizer, fit_normalizer, normal_only
from ml.synth import write_csv

SECOND = pd.Timedelta(seconds=1)


def _run(raw, events, features, seed=42):
    start = raw.index[0]
    return b.run(
        raw,
        events,
        features=features,
        t1=start + pd.Timedelta(days=2),
        t2=start + pd.Timedelta(days=3),
        gap_s=60,
        support_s=1.0,
        mapping=b.SYNTH_MAPPING,
        seed=seed,
    )


@pytest.fixture(scope="module")
def synth():
    # 4 synthetic days: train days 0–1, val day 2, test day 3 (midnight cuts).
    return b._synth_frame(4, 42)


@pytest.fixture(scope="module")
def runs(synth):
    raw, events = synth
    return {name: _run(raw, events, name) for name in b.FEATURE_SETS}


@pytest.fixture(scope="module")
def manifests(runs):
    return {
        name: b.build_manifest(result, name, "synthetic")
        for name, result in runs.items()
    }


# ------------------------------------------------------------ 1. features --
def test_feature_sets_are_section7_ordered_subsets():
    assert b.resolve_features("full") == (
        "pm2_5",
        "temp_c",
        "humidity_pct",
        "tvoc_ppb",
        "co_ppm",
    )
    assert b.resolve_features("pm_th") == ("pm2_5", "temp_c", "humidity_pct")
    assert b.resolve_features(["pm10", "co_ppm"]) == ("pm10", "co_ppm")
    for bad in ["nope", ["temp_c", "pm2_5"], ["pm2_5", "pm2_5"], [], ["label"]]:
        with pytest.raises(ValueError):
            b.resolve_features(bad)


def test_both_feature_sets_run_end_to_end(runs):
    for name, result in runs.items():
        assert result["model"]["features"] == list(b.FEATURE_SETS[name])
        assert set(result["results"]) == {
            "mahalanobis",
            "zmax",
            "zmax_fixed_3sigma",
        }


def test_pm_th_runs_without_ens160_and_co(synth):
    raw, events = synth
    raw = raw.copy()
    raw[["tvoc_ppb", "eco2_ppm", "co_ppm"]] = np.nan  # FW-5 not ready yet
    result = _run(raw, events, "pm_th")
    assert result["model"]["features"] == ["pm2_5", "temp_c", "humidity_pct"]
    assert result["results"]["mahalanobis"]["evaluated_points"] > 0
    with pytest.raises(ValueError, match="two normal values"):
        _run(raw, events, "full")


# ------------------------------------------------- 2. train-only, val-only --
def test_normalizer_is_split_py_fit_on_transformed_train_normal(synth, runs):
    raw, _ = synth
    train = raw.loc[raw.index < raw.index[0] + pd.Timedelta(days=2)]
    model = runs["pm_th"]["model"]
    features = model["features"]
    transformed = b.transform(train, features).assign(label=train.label)
    expected = fit_normalizer(transformed, columns=features)
    assert model["normalizer"] == expected
    np.testing.assert_array_equal(
        model["mean"], np.float32([expected["mean"][c] for c in features])
    )
    z = apply_normalizer(normal_only(transformed), expected)[features].dropna()
    np.testing.assert_allclose(model["center"], z.mean().to_numpy(), atol=1e-6)


def test_marked_train_rows_do_not_enter_fit(synth):
    raw, _ = synth
    train = raw.loc[raw.index < raw.index[0] + pd.Timedelta(days=2)].copy()
    reference = b.fit(train, "full")
    assert (train.label == "marked").any()
    train.loc[train.label == "marked", list(NUMERIC_COLUMNS)] = 1e6
    poisoned = b.fit(train, "full")
    for key in ("mean", "std", "center", "precision"):
        np.testing.assert_array_equal(reference[key], poisoned[key])


def test_val_and_test_cannot_change_fit_and_test_cannot_change_selection(synth, runs):
    raw, events = synth
    reference = runs["full"]
    t2 = raw.index[0] + pd.Timedelta(days=3)
    changed = raw.copy()
    later = changed.index >= t2
    changed.loc[later, "temp_c"] += 0.5
    changed.loc[later & (changed.label == "marked"), "pm2_5"] = 0
    result = _run(changed, events, "full")
    assert result["hash"] == reference["hash"]
    assert result["operating"] == reference["operating"]
    assert result["results"] != reference["results"]  # test really changed
    # Validation changes may move the threshold, never the fitted parameters.
    t1 = raw.index[0] + pd.Timedelta(days=2)
    changed = raw.copy()
    changed.loc[(raw.index >= t1) & (raw.index < t2), "pm2_5"] += 50
    result = _run(changed, events, "full")
    for key in ("mean", "std", "center", "precision"):
        np.testing.assert_array_equal(result["model"][key], reference["model"][key])


def test_thresholds_come_from_validation_or_fixed_rule(runs):
    for result in runs.values():
        sources = {s["threshold_source"] for s in result["results"].values()}
        assert sources == {"validation", "fixed"}
        assert result["results"]["zmax_fixed_3sigma"]["threshold"] == 3.0
        for method, op in result["operating"].items():
            assert result["results"][method]["threshold"] == op["threshold"]
            assert result["results"][method]["persistence_k"] == op["k"]
            assert np.float32(op["threshold"]) == op["threshold"]
            assert op["k"] in b.PERSISTENCE_GRID
            for row in op["table"]:
                fa = row["val_false_alarm_episodes_per_day"]
                assert fa is None or fa <= b.FA_BUDGET_PER_DAY


def test_selection_respects_budget_and_tie_break(synth):
    raw, events = synth
    val = raw.loc[raw.index >= raw.index[0] + pd.Timedelta(days=2)].iloc[:3600]
    ends = val.index + SECOND
    scores = np.zeros(len(val))
    scores[[100, 200, 300]] = 10.0  # three isolated normal spikes, no events
    op = b.select_operating_point(
        val, events.iloc[:0], scores, ends, b.SYNTH_MAPPING, budget=1e9, grid=(1, 3)
    )
    assert op["k"] == 1  # no anomaly events: smallest k wins
    strict = b.select_operating_point(
        val, events.iloc[:0], scores, ends, b.SYNTH_MAPPING, budget=0.0, grid=(1,)
    )
    assert strict["threshold"] > 10.0  # zero budget → above every normal score
    assert strict["table"][0]["stopped_by"] == "budget"
    assert op["table"][0]["stopped_by"] == "candidate_floor"


def test_events_crossing_split_boundaries_are_rejected(synth):
    raw, events = synth
    first = events.iloc[0]
    with pytest.raises(ValueError, match="crosses a split boundary"):
        _ = b.run(
            raw,
            events,
            features="pm_th",
            t1=first.start_iso + pd.Timedelta(seconds=30),
            t2=raw.index[0] + pd.Timedelta(days=3),
            gap_s=0,
            support_s=1.0,
            mapping=b.SYNTH_MAPPING,
            seed=42,
        )


# ---------------------------------------------------- 3. conditioning -----
def test_ledoit_wolf_matches_bruteforce_definition():
    rng = np.random.default_rng(7)
    x = rng.normal(size=(40, 4)) @ rng.normal(size=(4, 4))
    x -= x.mean(axis=0)
    s, shrunk, rho = b.ledoit_wolf(x)
    n, p = x.shape
    mu = np.trace(s) / p
    delta2 = np.sum((s - mu * np.eye(p)) ** 2) / p
    beta_bar2 = sum(np.sum((np.outer(r, r) - s) ** 2) for r in x) / p / n**2
    expected = min(beta_bar2, delta2) / delta2
    assert rho == pytest.approx(expected, rel=1e-12)
    assert 0 <= rho <= 1
    np.testing.assert_allclose(shrunk, rho * mu * np.eye(p) + (1 - rho) * s)
    assert np.linalg.cond(shrunk) <= np.linalg.cond(s)


def test_condition_numbers_and_regularization_are_reported(runs):
    for result in runs.values():
        diag = result["model"]["diagnostics"]
        assert 1 <= diag["cond_shrunk_z"] <= diag["cond_sample_z"] <= b.MAX_CONDITION
        assert 0 < diag["ledoit_wolf_shrinkage"] < 1e-3  # large n → tiny shrink
        assert diag["max_condition"] == b.MAX_CONDITION
        p = len(result["model"]["features"])
        assert np.array(diag["correlation"]).shape == (p, p)
        assert 1 <= diag["effective_dim_95"] <= p
        cov = np.array(diag["covariance_z"])
        assert np.linalg.eigvalsh(cov).min() > 0
        np.testing.assert_allclose(
            result["model"]["precision"] @ cov, np.eye(p), atol=1e-4
        )


def _toy(n=2000, seed=0):
    rng = np.random.default_rng(seed)
    index = pd.date_range("2026-09-23T00:00:00+07:00", periods=n, freq="s")
    frame = pd.DataFrame(
        {c: rng.normal(10, 1, n).round(1) for c in NUMERIC_COLUMNS}, index=index
    )
    frame["label"] = "normal"
    return frame


@pytest.mark.parametrize(
    "mutate",
    [
        lambda f: f.assign(humidity_pct=100 - 2 * f.temp_c),  # exact collinear
        lambda f: f.assign(co_ppm=0.4),  # zero std
    ],
)
def test_singular_covariance_is_rejected_not_pseudo_inverted(mutate):
    with pytest.raises(ValueError, match="condition number"):
        b.fit(mutate(_toy()), "full")


def test_condition_gate_uses_configured_limit():
    frame = _toy()
    frame["humidity_pct"] = frame.temp_c + np.random.default_rng(1).normal(
        0, 0.05, len(frame)
    )
    with pytest.raises(ValueError, match="condition number"):
        b.fit(frame, "full")
    diag = b.fit(frame, "full", max_condition=1e6)["diagnostics"]
    # Small n: shrinkage alone would pull cond under the gate and hide the
    # near-duplicate feature, which is why the gate uses the sample matrix.
    assert diag["cond_sample_z"] > b.MAX_CONDITION
    assert diag["cond_shrunk_z"] < diag["cond_sample_z"]


def test_unobserved_channel_and_too_few_rows_fail():
    with pytest.raises(ValueError):
        b.fit(_toy().assign(tvoc_ppb=np.nan), "full")
    with pytest.raises(ValueError, match="complete rows"):
        b.fit(_toy(n=20), "full")


# ------------------------------------------------------ missing policy ----
def test_transform_and_missing_policy_match_c_rules():
    frame = pd.DataFrame(
        {
            **{c: [10.0] * 6 for c in NUMERIC_COLUMNS},
            "label": "normal",
        }
    )
    frame.loc[1, "pm2_5"] = np.nan
    frame.loc[2, "pm2_5"] = -1.0
    frame.loc[3, "temp_c"] = np.inf
    frame.loc[4, "humidity_pct"] = -np.inf
    frame.loc[5, ["tvoc_ppb", "eco2_ppm", "co_ppm", "pm1_0", "pm10"]] = np.nan
    t = b.transform(frame, ("pm2_5", "temp_c", "humidity_pct"))
    assert t.pm2_5.iloc[0] == pytest.approx(math.log1p(10))
    assert t.isna().any(axis=1).tolist() == [False, True, True, True, True, False]
    model = {
        "features": ["pm2_5", "temp_c", "humidity_pct"],
        "mean": np.float32([2, 10, 10]),
        "std": np.float32([1, 1, 1]),
        "center": np.float32([0, 0, 0]),
        "precision": np.eye(3, dtype=np.float32),
    }
    for scorer in (b.score_d2, b.score_zmax):
        scores = scorer(model, frame)
        assert np.isfinite(scores).tolist() == [True, False, False, False, False, True]
        assert scores[0] == scores[5]  # unused columns cannot matter


def test_inf_in_train_is_counted_as_missing_not_fitted():
    frame = _toy()
    frame.iloc[5, frame.columns.get_loc("temp_c")] = np.inf
    model = b.fit(frame, "pm_th")
    assert model["diagnostics"]["invalid_to_missing"]["temp_c"] == 1
    assert model["diagnostics"]["fit_rows_complete"] == len(frame) - 1


# ----------------------------------------------------------- persistence --
def test_persistence_requires_contiguous_scored_rows():
    times = pd.date_range("2026-09-23T00:00:00+07:00", periods=8, freq="s")
    times = times.delete(5)  # gap between rows 4 and 5
    ends = times + SECOND
    scores = np.array([5, 5, 5, np.nan, 5, 5, 5], dtype=float)
    got = b.persist(scores, times, ends, 2)
    expected = [np.nan, 5, 5, np.nan, np.nan, np.nan, 5]
    np.testing.assert_array_equal(got, expected)
    np.testing.assert_array_equal(b.persist(scores, times, ends, 1), scores)
    with pytest.raises(ValueError):
        b.persist(scores, times, ends, 0)


def test_persistence_equals_board_counter():
    rng = np.random.default_rng(3)
    scores = rng.uniform(0, 2, 500)
    scores[rng.random(500) < 0.05] = np.nan
    times = pd.date_range("2026-09-23T00:00:00+07:00", periods=500, freq="s")
    for k in (1, 3, 5):
        persisted = b.persist(scores, times, times + SECOND, k)
        count, alarms = 0, []
        for value in scores:
            count = count + 1 if np.isfinite(value) and value >= 1.0 else 0
            alarms.append(count >= k)
        assert ((persisted >= 1.0) == np.array(alarms)).all()


# --------------------------------------------------------------- metrics --
def _keys(value):
    if isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from _keys(item)
    elif isinstance(value, list):
        for item in value:
            yield from _keys(item)


def test_metrics_come_from_ml2_without_accuracy(runs):
    for result in runs.values():
        for summary in result["results"].values():
            assert summary["definition_version"] == "ml2-v1"
            assert summary["event_count"] == 3
            assert {e["label"] for e in summary["events"]} == {
                "decorrelation",
                "spike",
                "drift",
            }
            assert "ap_raw_score_k1" in summary
        assert not any("accuracy" in str(k).lower() for k in _keys(result["results"]))


def test_mahalanobis_detects_decorrelation_that_3sigma_misses(runs):
    # สมมติฐานสำหรับข้อมูลสังเคราะห์: the ML-1 decorrelation recipe on 1 test day.
    for result in runs.values():
        by_label = {
            name: {e["label"]: e["detected"] for e in s["events"]}
            for name, s in result["results"].items()
        }
        assert by_label["mahalanobis"]["decorrelation"]
        assert not by_label["zmax_fixed_3sigma"]["decorrelation"]


def test_chi2_reference_matches_docs_table():
    for p, value in [(3, 16.27), (5, 20.52), (6, 22.46), (8, 26.12)]:
        assert b.chi2_isf(0.001, p) == pytest.approx(value, abs=0.01)
    assert b.chi2_isf(0.05, 1) == pytest.approx(3.841, abs=1e-3)
    assert b.chi2_isf(0.05, 2) == pytest.approx(5.991, abs=1e-3)


def test_seed_reproducible(synth, runs):
    raw, events = synth
    again = _run(raw, events, "pm_th")
    assert again["hash"] == runs["pm_th"]["hash"]
    assert again["results"] == runs["pm_th"]["results"]
    other_raw, other_events = b._synth_frame(4, 43)
    assert _run(other_raw, other_events, "pm_th")["hash"] != again["hash"]


# ---------------------------------------------------------------- export --
def _parse_header(text):
    def array(name):
        match = re.search(rf"{name}\[[^=]*=\s*\{{(.*?)\}};", text, re.DOTALL)
        body = match.group(1).replace("{", " ").replace("}", " ")
        items = [t.strip() for t in body.split(",") if t.strip()]
        out = []
        for item in items:
            item = item.strip('"').strip("()")
            if item in ("NAN",):
                out.append(np.nan)
            elif item in ("INFINITY", "-INFINITY"):
                out.append(np.inf if item == "INFINITY" else -np.inf)
            elif re.fullmatch(r"-?[0-9.e+-]+f", item):
                out.append(float(np.float32(item[:-1])))
            else:
                out.append(item if not item.isdigit() else int(item))
        return out

    def define(name):
        return re.search(rf"#define {name} (\S+)", text).group(1)

    return array, define


def test_header_matches_model_float32_exactly(manifests):
    for name, manifest in manifests.items():
        text = b.c_header(manifest, name)
        array, define = _parse_header(text)
        model = manifest["model"]
        p = len(model["features"])
        assert int(define("AG_MAHAL_N_FEATURES")) == p
        assert int(define("AG_MAHAL_N_NUMERIC")) == 8
        assert array("AG_MAHAL_FEATURE_NAMES") == model["features"]
        index = array("AG_MAHAL_NUMERIC_INDEX")
        assert index == [list(NUMERIC_COLUMNS).index(c) for c in model["features"]]
        assert index == sorted(index)  # §7 order
        for key in ("mean", "std", "center"):
            got = np.float32(array(f"AG_MAHAL_{key.upper()}"))
            np.testing.assert_array_equal(got, model[key])  # bit-exact float32
        got = np.float32(array("AG_MAHAL_PRECISION")).reshape(p, p)
        np.testing.assert_array_equal(got, model["precision"])
        assert np.float32(define("AG_MAHAL_THRESHOLD_D2")[:-1]) == np.float32(
            manifest["threshold_d2"]
        )
        assert int(define("AG_MAHAL_PERSISTENCE_K")) == manifest["persistence_k"]
        assert array("AG_MAHAL_TRANSFORM") == [
            b.TRANSFORM_CODES[t] for t in model["transforms"]
        ]
        golden = np.float32(array("AG_MAHAL_GOLDEN_INPUT")).reshape(-1, 8)
        assert len(golden) == int(define("AG_MAHAL_N_GOLDEN"))
        assert manifest["model_version"] in text


def test_golden_vectors_cover_cases_and_reproduce(manifests):
    for manifest in manifests.values():
        golden = manifest["golden"]
        names = [g["name"] for g in golden]
        for required in (
            "center_d2_zero",
            "boundary_at_threshold",
            "missing_feature",
            "infinite_feature",
            "negative_log1p_input",
            "unused_columns_missing",
        ):
            assert required in names
        assert any(n.startswith("val_decorrelation_max") for n in names)
        assert any(n.startswith("val_normal") for n in names)
        frame = pd.DataFrame(
            [g["input"].astype(float) for g in golden], columns=NUMERIC_COLUMNS
        )
        np.testing.assert_array_equal(
            b.score_d2(manifest["model"], frame), [g["d2"] for g in golden]
        )
        by = dict(zip(names, golden, strict=True))
        assert by["center_d2_zero"]["d2"] == pytest.approx(0, abs=1e-6)
        for name in ("missing_feature", "infinite_feature", "negative_log1p_input"):
            assert np.isnan(by[name]["d2"]) and not by[name]["alarm_row"]
        assert by["boundary_at_threshold"]["near_threshold"]
        assert by["boundary_double_threshold"]["alarm_row"]
        assert not by["boundary_half_threshold"]["alarm_row"]


def _float32_d2(model, row):
    """Emulate the C loop in float32 arithmetic."""
    d = []
    for j, column in enumerate(model["features"]):
        v = np.float32(row[list(NUMERIC_COLUMNS).index(column)])
        if not np.isfinite(v):
            return np.nan
        if b.transform_name(column) == "log1p":
            if not v >= 0:
                return np.nan
            v = np.float32(np.log1p(v))
        d.append(
            np.float32((v - model["mean"][j]) / model["std"][j]) - model["center"][j]
        )
    acc = np.float32(0)
    for i, di in enumerate(d):
        for j, dj in enumerate(d):
            acc = np.float32(acc + np.float32(di * model["precision"][i, j]) * dj)
    return float(acc)


def test_float32_arithmetic_stays_within_tolerance(manifests, runs):
    for name, manifest in manifests.items():
        model = manifest["model"]
        val = runs[name]["val"]
        rows = [g["input"] for g in manifest["golden"]]
        rows += list(val[list(NUMERIC_COLUMNS)].dropna().iloc[::997].to_numpy())
        frame = pd.DataFrame(np.float32(rows).astype(float), columns=NUMERIC_COLUMNS)
        reference = b.score_d2(model, frame)
        for row, ref in zip(np.float32(rows), reference, strict=True):
            got = _float32_d2(model, row)
            if np.isnan(ref):
                assert np.isnan(got)
            else:
                assert abs(got - ref) <= b.tolerance(ref)


def test_persistence_fixture_matches_counter(manifests):
    for manifest in manifests.values():
        seq = manifest["persistence_sequence"]
        k, threshold = manifest["persistence_k"], manifest["threshold_d2"]
        count, alarms = 0, []
        for value in seq["d2"]:
            count = count + 1 if np.isfinite(value) and value >= threshold else 0
            alarms.append(count >= k)
        assert alarms == seq["alarm"] and any(alarms) and not all(alarms)


C_CHECK = r"""
#include <stdio.h>
#include HEADER
static float d2(const float *x) {
    float d[AG_MAHAL_N_FEATURES];
    for (int j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        float v = x[AG_MAHAL_NUMERIC_INDEX[j]];
        if (!isfinite(v)) return NAN;
        if (AG_MAHAL_TRANSFORM[j] == AG_MAHAL_TRANSFORM_LOG1P) {
            if (!(v >= 0.0f)) return NAN;
            v = log1pf(v);
        }
        d[j] = (v - AG_MAHAL_MEAN[j]) / AG_MAHAL_STD[j] - AG_MAHAL_CENTER[j];
    }
    float acc = 0.0f;
    for (int i = 0; i < AG_MAHAL_N_FEATURES; ++i)
        for (int j = 0; j < AG_MAHAL_N_FEATURES; ++j)
            acc += d[i] * AG_MAHAL_PRECISION[i][j] * d[j];
    return acc;
}
int main(void) {
    int bad = 0;
    for (int g = 0; g < AG_MAHAL_N_GOLDEN; ++g) {
        float got = d2(AG_MAHAL_GOLDEN_INPUT[g]), ref = AG_MAHAL_GOLDEN_D2[g];
        if (isnan(ref)) { bad += !isnan(got); continue; }
        float err = fabsf(got - ref);
        bad += !(err <= AG_MAHAL_TOL_ABS + AG_MAHAL_TOL_REL * fabsf(ref));
        if (!AG_MAHAL_GOLDEN_NEAR_THRESHOLD[g])
            bad += (got >= AG_MAHAL_THRESHOLD_D2) != AG_MAHAL_GOLDEN_ALARM_ROW[g];
        printf("%d %.9g %.9g\n", g, (double)got, (double)err);
    }
    int count = 0;
    for (int i = 0; i < AG_MAHAL_N_SEQUENCE; ++i) {
        float v = AG_MAHAL_SEQUENCE_D2[i];
        count = (!isnan(v) && v >= AG_MAHAL_THRESHOLD_D2) ? count + 1 : 0;
        bad += (count >= AG_MAHAL_PERSISTENCE_K) != AG_MAHAL_SEQUENCE_ALARM[i];
    }
    printf("bad %d\n", bad);
    return bad != 0;
}
"""


@pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not on PATH")
def test_header_compiles_and_c_float32_matches_golden(manifests, tmp_path):
    for name, manifest in manifests.items():
        header = tmp_path / f"ag_mahal_{name}.h"
        header.write_text(b.c_header(manifest, name), encoding="utf-8")
        source = tmp_path / f"check_{name}.c"
        source.write_text(C_CHECK.replace("HEADER", f'"{header.name}"'))
        exe = tmp_path / f"check_{name}.exe"
        subprocess.run(
            [
                "gcc",
                "-std=c99",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(exe),
                "-lm",
            ],
            check=True,
            cwd=tmp_path,
            capture_output=True,
        )
        result = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stdout
        assert result.stdout.strip().endswith("bad 0")


def test_export_writes_artifacts_under_data_processed(runs, output_dir):
    manifest, header = b.export(
        runs["pm_th"], output_dir / "pm_th", "pm_th", "synthetic"
    )
    folder = output_dir / "pm_th"
    data = json.loads((folder / "model.json").read_text(encoding="utf-8"))
    assert data["model_version"] == manifest["model_version"]
    assert data["feature_numeric_index"] == [1, 3, 4]
    assert data["model"]["features"] == ["pm2_5", "temp_c", "humidity_pct"]
    assert data["threshold_d2"] == manifest["threshold_d2"]
    assert data["golden"][0]["input"][0] is not None
    assert (
        json.loads((folder / "norm.json").read_text())["columns"]
        == data["model"]["features"]
    )
    results = json.loads((folder / "results.json").read_text(encoding="utf-8"))
    assert results["frozen_sha256"] == runs["pm_th"]["hash"]
    assert (folder / "test-mahalanobis" / "metrics.json").exists()
    assert header.read_text(encoding="utf-8").startswith("/* Auto-generated")
    with pytest.raises(ValueError):
        b.export(runs["pm_th"], output_dir.parents[2] / "elsewhere", "pm_th", "x")


def test_cli_synthetic_and_real_paths(output_dir, capsys):
    b.main(["--days", "3", "--feature-set", "pm_th", "--out", str(output_dir / "cli")])
    assert (output_dir / "cli" / "pm_th" / "ag_mahal_pm_th.h").exists()
    raw, events = b.generate(days=3, seed=5)
    raw_path, events_path = write_csv(raw, events, output_dir / "csv")
    b.main(
        [
            "--raw",
            str(raw_path),
            "--events",
            str(events_path),
            "--mapping",
            "decorrelation=anomaly,spike=anomaly,drift=anomaly",
            "--row-support-s",
            "1",
            "--t1",
            "2026-09-24T00:00:00+07:00",
            "--t2",
            "2026-09-25T00:00:00+07:00",
            "--feature-set",
            "pm_th",
            "--out",
            str(output_dir / "real"),
        ]
    )
    data = json.loads(
        (output_dir / "real" / "pm_th" / "model.json").read_text(encoding="utf-8")
    )
    assert data["data_source"] == "real"
    assert "test mahalanobis" in capsys.readouterr().out
    with pytest.raises(SystemExit):
        b.main(["--raw", str(raw_path), "--out", str(output_dir / "x")])


# ------------------------------------------------- review b98c366 R1–R4 ----
def test_pm_th_export_works_without_ens160_and_co(synth, output_dir):
    """R1: unused §7 columns empty in every row must not break export."""
    raw, events = synth
    raw = raw.copy()
    raw[["tvoc_ppb", "eco2_ppm", "co_ppm"]] = np.nan
    result = _run(raw, events, "pm_th")
    manifest, header = b.export(result, output_dir / "sparse", "pm_th", "synthetic")
    golden = manifest["golden"]
    inputs = np.array([g["input"] for g in golden], dtype=float)
    assert np.isnan(inputs[:, [5, 6, 7]]).all()  # unused columns stay missing
    by = {g["name"]: g for g in golden}
    assert by["center_d2_zero"]["d2"] == pytest.approx(0, abs=1e-6)
    assert np.isfinite([g["d2"] for g in golden if g["name"].startswith("val_")]).all()
    frame = pd.DataFrame(inputs, columns=NUMERIC_COLUMNS)
    np.testing.assert_array_equal(
        b.score_d2(manifest["model"], frame), [g["d2"] for g in golden]
    )
    assert "NAN" in header.read_text(encoding="utf-8")


@pytest.mark.parametrize("value", [np.nan, np.inf, -np.inf])
def test_persist_all_missing_returns_all_nan(value):
    """R2: no finite score at all → NaN everywhere, no exception."""
    times = pd.date_range("2026-09-23T00:00:00+07:00", periods=4, freq="s")
    for k in (1, 3):
        got = b.persist(np.full(4, value), times, times + SECOND, k)
        assert np.isnan(got).all()
    empty = times[:0]
    assert b.persist(np.array([]), empty, empty + SECOND, 1).size == 0


def test_test_split_with_all_scores_missing_still_evaluates(synth):
    """R2: sensor offline for the whole test split → metrics, not a crash."""
    raw, events = synth
    raw = raw.copy()
    raw.loc[raw.index >= raw.index[0] + pd.Timedelta(days=3), "pm2_5"] = np.nan
    result = _run(raw, events, "pm_th")
    for summary in result["results"].values():
        assert summary["evaluated_points"] == 0
        assert summary["missing_score_points"] == summary["points"]
        assert summary["detected_events"] == 0
        assert summary["missed_events"] == summary["event_count"] == 3
        assert summary["ap"] is None and summary["tp"] == summary["fp"] == 0


def _boundary_run(raw, start, end, gap_s=60):
    event = pd.DataFrame(
        [{"start_iso": start, "end_iso": end, "label": "spike", "note": ""}]
    )
    marked = raw.copy()
    marked.loc[(marked.index >= start) & (marked.index < end), "label"] = "marked"
    return b.run(
        marked,
        event,
        features="pm_th",
        t1=raw.index[2400],
        t2=raw.index[4800],
        gap_s=gap_s,
        support_s=1.0,
        mapping={"spike": "anomaly"},
        seed=42,
        grid=(1,),
    )


@pytest.mark.parametrize(
    ("start", "end"),
    [
        (2390, 2410),  # crosses t1, ends inside the gap (review probe)
        (2410, 2420),  # wholly inside the t1 gap
        (4790, 4900),  # crosses t2 into test
        (2440, 2470),  # starts in the gap, ends inside val
    ],
)
def test_events_crossing_boundaries_or_gaps_are_rejected(start, end):
    """R3: no event may be dropped silently between splits."""
    raw = _toy(n=7200)
    with pytest.raises(ValueError, match="crosses a split boundary"):
        _boundary_run(raw, raw.index[start], raw.index[end])


def test_events_touching_boundaries_are_accepted():
    raw = _toy(n=7200)
    t1, t2 = raw.index[2400], raw.index[4800]
    in_train = _boundary_run(raw, raw.index[2300], t1)  # ends exactly at t1
    assert in_train["split"]["val"]["events"] == 0
    in_val = _boundary_run(raw, t1 + pd.Timedelta(seconds=60), raw.index[2500])
    assert in_val["split"]["val"]["events"] == 1
    in_test = _boundary_run(raw, t2 + pd.Timedelta(seconds=60), raw.index[4900])
    assert in_test["split"]["test"]["events"] == 1


def test_no_validation_events_selects_smallest_k_even_if_fa_differs():
    """R4: without anomaly events k=min(grid), as documented."""
    times = pd.date_range("2026-09-23T00:00:00+07:00", periods=3600, freq="s")
    frame = pd.DataFrame({"label": "normal"}, index=times)
    scores = np.random.default_rng(0).normal(size=len(times))
    empty = pd.DataFrame(columns=["start_iso", "end_iso", "label", "note"])
    op = b.select_operating_point(
        frame, empty, scores, times + SECOND, {}, budget=1e9, grid=(3, 1, 5)
    )
    fa = {row["k"]: row["val_false_alarm_episodes_per_day"] for row in op["table"]}
    assert fa[1] != fa[3]  # FA alone would not pick k=1 here
    assert op["k"] == 1
