"""ML-3 baselines: univariate 3σ (max |z|) vs Mahalanobis D², plus C export.

Pipeline (see ml/BASELINE.md for the reasons behind each choice):
1. chronological_split (ml/split.py); fit uses train rows with raw label
   normal only — marked rows never enter the fit.
2. Per-feature transform (log1p for PM/TVOC, identity otherwise), then the
   ml/split.py normalizer fitted on train normal rows only.
3. Covariance on complete train-normal rows in z-space, Ledoit–Wolf
   shrinkage, condition-number gate, explicit inverse. Parameters are rounded
   to float32 *before* any scoring so Python evaluates the deployed model.
4. Threshold and persistence chosen on validation only (false-alarm budget,
   ml/metrics.py), then frozen and hashed before the test split is scored.
5. Test evaluated once with ml/metrics.py; never accuracy, never point
   adjustment.

Synthetic data are สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง and never
evidence of sensor accuracy. Feature order is always the CLAUDE.md §7 order.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd

try:
    from .load import NUMERIC_COLUMNS, load_events, load_raw
    from .metrics import evaluate, save_result
    from .split import (
        _check_columns,
        apply_normalizer,
        chronological_split,
        fit_normalizer,
        normal_only,
        save_normalizer,
    )
    from .synth import generate
except ImportError:  # `python ml/baseline.py`
    from load import NUMERIC_COLUMNS, load_events, load_raw
    from metrics import evaluate, save_result
    from split import (
        _check_columns,
        apply_normalizer,
        chronological_split,
        fit_normalizer,
        normal_only,
        save_normalizer,
    )
    from synth import generate

MODEL_VERSION = "ml3-v1"
DAY_S = 86400

# ชุด feature: "full" = หนึ่งช่องต่อแหล่งข้อมูลอิสระ (PM · T · RH · VOC · CO)
# ตัด pm1_0/pm10 (เซนเซอร์ตัวเดียวกับ pm2_5) และ eco2_ppm (ENS160 คำนวณจาก TVOC)
# ตาม docs/05-MULTIVARIATE.md §3.1; "pm_th" = ใช้ได้ก่อน ENS160/ZE07-CO พร้อม
FEATURE_SETS = {
    "full": ("pm2_5", "temp_c", "humidity_pct", "tvoc_ppb", "co_ppm"),
    "pm_th": ("pm2_5", "temp_c", "humidity_pct"),
}
# Right-skewed counts (docs/05 §3.2). Domain x >= 0; anything else is missing.
LOG1P_COLUMNS = ("pm1_0", "pm2_5", "pm10", "tvoc_ppb")
TRANSFORM_CODES = {"identity": 0, "log1p": 1}

# float32: eps = 2^-23 ≈ 1.19e-7. Relative error of a float32 quadratic form is
# bounded by about 2·p·eps·cond(Σ); cond ≤ 1e3 and p ≤ 8 keeps it ≤ ~2e-3 and
# matches docs/05 §3.1 where ρ = 0.999 (cond ≈ 2e3) is already unusable.
MAX_CONDITION = 1e3
MIN_ROWS_PER_FEATURE = 10  # docs/05 §3.4: n >= 10p
FA_BUDGET_PER_DAY = 1.0  # สมมติฐานด้านการใช้งาน: เตือนผิด ≤ 1 ครั้ง/วัน
PERSISTENCE_GRID = (1, 3, 5, 10)  # consecutive 1 s rows
CLASSIC_SIGMA = 3.0
# Python (float64 on float32 params) vs C (float32 arithmetic) for one D².
TOL_ABS = 1e-3
TOL_REL = 1e-3
NO_ALARM = float(np.finfo(np.float64).max)
SYNTH_MAPPING = {"decorrelation": "anomaly", "spike": "anomaly", "drift": "anomaly"}


# ---------------------------------------------------------------- features --
def resolve_features(features):
    """Feature-set name or column list → §7-ordered column tuple."""
    if isinstance(features, str):
        if features not in FEATURE_SETS:
            raise ValueError(f"unknown feature set {features!r}")
        features = FEATURE_SETS[features]
    return tuple(_check_columns(features))


def transform_name(column):
    return "log1p" if column in LOG1P_COLUMNS else "identity"


def transform(df, features):
    """Apply per-feature transforms; invalid or non-finite input → NaN.

    Same rule as C: identity needs isfinite(x); log1p needs isfinite(x) and
    x >= 0. Values are never imputed, clipped or replaced by zero.
    """
    out = pd.DataFrame(index=df.index)
    for column in features:
        values = df[column].to_numpy(dtype=float, na_value=np.nan)
        valid = np.isfinite(values)
        if transform_name(column) == "log1p":
            valid &= values >= 0
            with np.errstate(invalid="ignore"):
                values = np.log1p(np.where(valid, values, 0.0))
        out[column] = np.where(valid, values, np.nan)
    return out


# --------------------------------------------------------------- covariance --
def ledoit_wolf(x):
    """Ledoit & Wolf (2004) shrinkage toward μI for centered rows x (n×p).

    S = xᵀx/n, μ = tr(S)/p, δ² = ‖S − μI‖²/p,
    β̄² = (Σ_k ‖x_k‖⁴ − n‖S‖²)/(p n²), β² = min(β̄², δ²), ρ = β²/δ²,
    Σ* = ρμI + (1 − ρ)S. Same estimator as sklearn.covariance.LedoitWolf.
    Returns (S, Σ*, ρ).
    """
    n, p = x.shape
    s = x.T @ x / n
    mu = np.trace(s) / p
    delta2 = np.sum((s - mu * np.eye(p)) ** 2) / p
    beta_bar2 = (np.sum(np.sum(x**2, axis=1) ** 2) - n * np.sum(s**2)) / (p * n**2)
    rho = 0.0 if delta2 == 0 else float(min(beta_bar2, delta2) / delta2)
    return s, rho * mu * np.eye(p) + (1 - rho) * s, rho


def _condition(matrix):
    eig = np.linalg.eigvalsh(matrix)
    return float(eig[-1] / eig[0]) if eig[0] > 0 else math.inf, eig


def fit(train, features, max_condition=MAX_CONDITION):
    """Fit transforms → normalizer → shrunk covariance on train normal rows.

    Returns a float32-rounded model dict plus float64 diagnostics. Raises
    ValueError when the covariance is (near) singular; pseudo-inverse is not
    used because it gives zero weight to the near-null directions where
    broken correlations show up.
    """
    features = resolve_features(features)
    normal = normal_only(train)
    transformed = transform(normal, features)
    invalid = {
        c: int((normal[c].notna() & transformed[c].isna()).sum()) for c in features
    }
    transformed["label"] = normal["label"]
    norm = fit_normalizer(transformed, columns=features)
    z = apply_normalizer(transformed, norm).loc[:, list(features)]
    complete = z.dropna().to_numpy(dtype=float)
    n, p = complete.shape
    if n < MIN_ROWS_PER_FEATURE * p:
        raise ValueError(f"need >= {MIN_ROWS_PER_FEATURE * p} complete rows, got {n}")
    center = complete.mean(axis=0)
    sample, shrunk, rho = ledoit_wolf(complete - center)
    cond_sample, eig_sample = _condition(sample)
    cond_shrunk, _ = _condition(shrunk)
    raw_units = transformed.loc[:, list(features)].dropna().to_numpy(dtype=float)
    cond_raw_units, _ = _condition(np.cov(raw_units.T, ddof=0).reshape(p, p))
    # Gate on the *sample* matrix: shrinkage must not hide a constant or
    # collinear feature (with large n it is ~0 anyway, see diagnostics).
    if not max(cond_sample, cond_shrunk) <= max_condition:
        raise ValueError(
            f"covariance condition number {cond_sample:.4g} > {max_condition:g} "
            f"(eigenvalues {eig_sample.tolist()}); remove constant or collinear "
            "features instead of inverting a near-singular matrix"
        )
    precision = np.linalg.inv(shrunk)
    precision = (precision + precision.T) / 2
    corr = np.corrcoef(complete.T).reshape(p, p)
    explained = np.cumsum(eig_sample[::-1]) / eig_sample.sum()
    f32 = lambda a: np.asarray(a, dtype=np.float32)
    return {
        "features": list(features),
        "transforms": [transform_name(c) for c in features],
        "mean": f32([norm["mean"][c] for c in features]),
        "std": f32([norm["std"][c] for c in features]),
        "center": f32(center),
        "precision": f32(precision),
        "diagnostics": {
            "fit_rows_train_normal": len(normal),
            "fit_rows_complete": int(n),
            "invalid_to_missing": invalid,
            "ledoit_wolf_shrinkage": rho,
            "cond_sample_z": cond_sample,
            "cond_shrunk_z": cond_shrunk,
            "cond_raw_units_transformed": cond_raw_units,
            "max_condition": max_condition,
            "eigenvalues_sample_z": eig_sample.tolist(),
            "correlation": corr.tolist(),
            "effective_dim_95": int(np.searchsorted(explained, 0.95) + 1),
            "covariance_z": shrunk.tolist(),
        },
        "normalizer": norm,
    }


# ------------------------------------------------------------------ scoring --
def _z(model, df):
    t = transform(df, model["features"]).to_numpy(dtype=float)
    mean = model["mean"].astype(float)
    std = model["std"].astype(float)
    return (t - mean) / std  # NaN propagates; params are the float32 export


def score_d2(model, df):
    """Mahalanobis D² (squared, unitless). NaN if any selected feature missing."""
    d = _z(model, df) - model["center"].astype(float)
    p = model["precision"].astype(float)
    return np.einsum("ni,ij,nj->n", d, p, d)


def score_zmax(model, df):
    """Univariate baseline: max_j |z_j|; same missing policy as D²."""
    z = np.abs(_z(model, df))
    out = np.full(len(z), np.nan)
    ok = np.isfinite(z).all(axis=1)
    out[ok] = z[ok].max(axis=1)
    return out


SCORERS = {"mahalanobis": score_d2, "zmax": score_zmax}


def persist(scores, times, ends, k):
    """Score that crosses a threshold iff the last k rows all do.

    Returns the min over k consecutive scored rows with contiguous support
    (times[i] == ends[i-1]); otherwise NaN. Equivalent C: a counter that
    increments on score >= threshold, resets on below/missing/gap, alarms
    at counter >= k.
    """
    scores = np.asarray(scores, dtype=float)
    if k < 1 or int(k) != k:
        raise ValueError("persistence k must be a positive integer")
    finite = np.isfinite(scores)
    contiguous = np.r_[False, np.asarray(times[1:] == ends[:-1])]
    starts = finite & ~(contiguous & np.r_[False, finite[:-1]])
    index = np.arange(len(scores))
    # Index of the latest run start; every finite row has one (-1 otherwise).
    last_start = np.maximum.accumulate(np.where(starts, index, -1))
    run = np.where(finite, index - last_start + 1, 0)
    rolled = pd.Series(scores).rolling(int(k), min_periods=int(k)).min().to_numpy()
    return np.where(run >= k, rolled, np.nan)


# ------------------------------------------------------------- evaluation --
def check_event_boundaries(events, t1, t2, gap_s):
    """Every event must lie wholly in train (< t1), val [t1+gap, t2) or
    test (>= t2+gap). Events straddling t1/t2 or touching a gap → error,
    so no annotation silently disappears between splits."""
    if events.empty:
        return
    t1, t2 = pd.Timestamp(t1), pd.Timestamp(t2)
    gap = pd.Timedelta(seconds=gap_s)
    start = pd.to_datetime(events.start_iso)
    end = pd.to_datetime(events.end_iso)
    ok = (end <= t1) | ((start >= t1 + gap) & (end <= t2)) | (start >= t2 + gap)
    if not ok.all():
        bad = events.loc[~ok, ["start_iso", "end_iso", "label"]].head(3)
        raise ValueError(
            "an event crosses a split boundary or gap; move t1/t2 or gap: "
            f"{bad.to_dict('records')}"
        )


def split_events(events, frame, ends):
    """Events fully inside this split's observed support; partial → error."""
    if events.empty:
        return events.copy()
    start = pd.to_datetime(events.start_iso)
    end = pd.to_datetime(events.end_iso)
    left, right = frame.index[0], ends[-1]
    inside = (start >= left) & (end <= right)
    outside = (end <= left) | (start >= right)
    if (~inside & ~outside).any():
        raise ValueError("an event crosses a split boundary; move t1/t2 or gap")
    return events.loc[inside].reset_index(drop=True)


def run_metrics(frame, events, scores, ends, mapping, threshold, source):
    return evaluate(
        frame,
        pd.Series(scores, index=frame.index),
        events,
        observed_until=pd.Series(ends, index=frame.index),
        event_mapping=mapping,
        threshold=float(threshold),
        threshold_source=source,
    )


def select_operating_point(
    frame,
    events,
    raw_scores,
    ends,
    mapping,
    budget=FA_BUDGET_PER_DAY,
    grid=PERSISTENCE_GRID,
):
    """Choose (threshold, k) from validation only.

    For each k: scan float32 thresholds from just above the largest val
    known-normal persisted score downward (normal quantiles 1−1e-6 … 0.5) and
    keep the lowest one before false-alarm episodes/day first exceeds the
    budget; stopped_by records whether the budget or the median floor ended it.
    Then pick k by val event detection rate ↓, MTTD ↑, false alarms ↑, k ↑.
    Without val anomaly events (detection rate undefined for every k) the
    smallest k wins regardless of false alarms, keeping alarm latency short.
    """
    rows = []
    for k in grid:
        scores = persist(raw_scores, frame.index, ends, k)
        # Highest finite threshold → no alarms; only the truth column is used.
        _, audit = run_metrics(
            frame, events, scores, ends, mapping, NO_ALARM, "validation"
        )
        normal = scores[(audit.truth == "normal").to_numpy() & np.isfinite(scores)]
        if normal.size == 0:
            raise ValueError("validation has no scored normal rows")
        top = np.nextafter(np.float32(normal.max()), np.float32(np.inf))
        levels = 1 - np.logspace(np.log10(0.5), -6, 31)
        candidates = np.unique(
            np.r_[top, np.quantile(normal, levels).astype(np.float32)]
        )[::-1]
        chosen, stopped_by = None, "candidate_floor"
        for threshold in candidates:
            summary, _ = run_metrics(
                frame, events, scores, ends, mapping, threshold, "validation"
            )
            rate = summary["false_alarm_episodes_per_day"]
            if rate is not None and rate > budget:
                stopped_by = "budget"
                break
            chosen = (float(threshold), summary)
        threshold, summary = chosen
        rows.append(
            {
                "k": int(k),
                "threshold": threshold,
                "val_event_detection_rate": summary["event_detection_rate"],
                "val_mttd_detected_s": summary["mttd_detected_s"],
                "val_false_alarm_episodes_per_day": summary[
                    "false_alarm_episodes_per_day"
                ],
                "val_ap": summary["ap"],
                "stopped_by": stopped_by,
            }
        )

    def key(row):
        rate = row["val_event_detection_rate"] or 0.0
        mttd = row["val_mttd_detected_s"]
        fa = row["val_false_alarm_episodes_per_day"] or 0.0
        return (-rate, math.inf if mttd is None else mttd, fa, row["k"])

    if all(row["val_event_detection_rate"] is None for row in rows):
        best = min(rows, key=lambda row: row["k"])  # nothing to trade latency for
    else:
        best = min(rows, key=key)
    return {"threshold": best["threshold"], "k": best["k"], "table": rows}


def frozen_hash(model, operating):
    payload = {
        "version": MODEL_VERSION,
        "features": model["features"],
        "transforms": model["transforms"],
        **{key: model[key].tolist() for key in ("mean", "std", "center", "precision")},
        "operating": {m: (o["threshold"], o["k"]) for m, o in operating.items()},
    }
    text = json.dumps(payload, sort_keys=True, allow_nan=False)
    return hashlib.sha256(text.encode()).hexdigest()


def chi2_sf(x, p):
    """Upper tail of χ²(p) for integer p, closed form (no SciPy)."""
    h = x / 2
    if p % 2 == 0:
        terms = sum(h**k / math.factorial(k) for k in range(p // 2))
        return math.exp(-h) * terms
    terms = sum(h ** (k - 0.5) / math.gamma(k + 0.5) for k in range(1, (p + 1) // 2))
    return math.erfc(math.sqrt(h)) + math.exp(-h) * terms


def chi2_isf(alpha, p):
    """Critical value c with P(χ²(p) > c) = alpha, by bisection."""
    lo, hi = 0.0, 1000.0
    for _ in range(200):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if chi2_sf(mid, p) > alpha else (lo, mid)
    return (lo + hi) / 2


def run(
    frame,
    events,
    *,
    features,
    t1,
    t2,
    gap_s,
    support_s,
    mapping,
    seed,
    budget=FA_BUDGET_PER_DAY,
    grid=PERSISTENCE_GRID,
    max_condition=MAX_CONDITION,
):
    """Fit on train → select on val → freeze → evaluate test once."""
    if not support_s or not np.isfinite(support_s) or support_s <= 0:
        raise ValueError("support_s must come from recording evidence (> 0)")
    train, val, test = chronological_split(frame, t1, t2, gap_s)
    check_event_boundaries(events, t1, t2, gap_s)
    support = pd.Timedelta(seconds=support_s)
    model = fit(train, features, max_condition)
    val_ends = val.index + support
    val_events = split_events(events, val, val_ends)
    operating, val_scores = {}, {}
    for method, scorer in SCORERS.items():
        val_scores[method] = scorer(model, val)
        operating[method] = select_operating_point(
            val, val_events, val_scores[method], val_ends, mapping, budget, grid
        )
    digest = frozen_hash(model, operating)  # frozen before any test scoring
    test_ends = test.index + support
    test_events = split_events(events, test, test_ends)
    results = {}
    plans = [
        (m, operating[m]["threshold"], operating[m]["k"], "validation") for m in SCORERS
    ]
    plans.append(("zmax_fixed_3sigma", CLASSIC_SIGMA, 1, "fixed"))
    test_audit = None
    for name, threshold, k, source in plans:
        raw_scores = SCORERS[name.split("_fixed")[0]](model, test)
        scores = persist(raw_scores, test.index, test_ends, k)
        summary, audit = run_metrics(
            test, test_events, scores, test_ends, mapping, threshold, source
        )
        ranking, _ = run_metrics(
            test, test_events, raw_scores, test_ends, mapping, threshold, source
        )
        summary["ap_raw_score_k1"] = ranking["ap"]
        summary["persistence_k"] = k
        results[name] = summary
        if name == "mahalanobis":
            test_audit = audit
    p = len(model["features"])
    chi2_ref = chi2_isf(0.001, p)
    val_normal = val["label"].eq("normal").to_numpy() & np.isfinite(
        val_scores["mahalanobis"]
    )
    return {
        "model": model,
        "operating": operating,
        "hash": digest,
        "results": results,
        "test_audit": test_audit,
        "split": {
            name: {
                "rows": len(part),
                "start": part.index[0].isoformat(),
                "end": part.index[-1].isoformat(),
                "events": len(evs) if evs is not None else None,
            }
            for name, part, evs in (
                ("train", train, None),
                ("val", val, val_events),
                ("test", test, test_events),
            )
        },
        "chi2_reference": {
            "alpha": 0.001,
            "p": p,
            "d2": chi2_ref,
            "val_raw_normal_fraction_above": float(
                np.mean(val_scores["mahalanobis"][val_normal] >= chi2_ref)
            ),
        },
        "val": val,
        "val_scores": val_scores["mahalanobis"],
        "val_events": val_events,
        "seed": seed,
        "budget": budget,
        "grid": list(grid),
        "mapping": mapping,
    }


# ------------------------------------------------------------------- export --
def tolerance(reference):
    return TOL_ABS + TOL_REL * abs(reference)


def golden_vectors(model, threshold, val, val_scores, val_events, seed):
    """Deterministic C fixtures: (name, §7 numeric row float32, D² reference).

    Uses only validation rows plus constructed points; never test rows.
    """
    rng = np.random.default_rng(seed)
    cols = list(NUMERIC_COLUMNS)
    idx = [cols.index(c) for c in model["features"]]
    complete = val[list(model["features"])].notna().all(axis=1).to_numpy()
    if not complete.any():
        raise ValueError("validation has no row with every selected feature")
    # Unused columns keep whatever the row holds, including NaN (pre-FW-5).
    template = val[cols].iloc[int(np.argmax(complete))].to_numpy(dtype=float)
    vectors = []

    def from_z(zvec):
        row = template.copy()
        t = model["mean"].astype(float) + model["std"].astype(float) * zvec
        for j, column in enumerate(model["features"]):
            row[idx[j]] = np.expm1(t[j]) if transform_name(column) == "log1p" else t[j]
        return row

    center = model["center"].astype(float)
    vectors.append(("center_d2_zero", from_z(center)))
    finite = np.flatnonzero(np.isfinite(val_scores) & val["label"].eq("normal"))
    for i in sorted(rng.choice(finite, size=min(6, finite.size), replace=False)):
        vectors.append((f"val_normal_{val.index[i].isoformat()}", val[cols].iloc[i]))
    # Highest-D² validation row of each annotated event kind.
    for label, group in val_events.groupby("label", sort=True):
        mask = np.zeros(len(val), dtype=bool)
        for event in group.itertuples(index=False):
            mask |= (val.index >= pd.Timestamp(event.start_iso)) & (
                val.index < pd.Timestamp(event.end_iso)
            )
        scores = np.where(mask & np.isfinite(val_scores), val_scores, -np.inf)
        if np.isfinite(scores).any():
            i = int(np.argmax(scores))
            vectors.append(
                (f"val_{label}_max_{val.index[i].isoformat()}", val[cols].iloc[i])
            )
    # Boundary points on the ray center + s·(1,…,1) in z-space with D² = s²·base.
    direction = np.ones(len(idx))
    precision = model["precision"].astype(float)
    base = direction @ precision @ direction
    for scale, name in ((0.5, "half"), (1.0, "at"), (2.0, "double")):
        zvec = center + direction * math.sqrt(scale * threshold / base)
        vectors.append((f"boundary_{name}_threshold", from_z(zvec)))
    missing = template.copy()
    missing[idx[0]] = np.nan
    vectors.append(("missing_feature", missing))
    inf = template.copy()
    inf[idx[-1]] = np.inf
    vectors.append(("infinite_feature", inf))
    log_cols = [
        i
        for i, c in zip(idx, model["features"], strict=True)
        if transform_name(c) == "log1p"
    ]
    if log_cols:
        negative = template.copy()
        negative[log_cols[0]] = -1.0
        vectors.append(("negative_log1p_input", negative))
    unused = [i for i in range(len(cols)) if i not in idx]
    if unused:
        sparse = template.copy()
        sparse[unused] = np.nan
        vectors.append(("unused_columns_missing", sparse))
    out = []
    for name, row in vectors:
        row32 = np.asarray(row, dtype=np.float64).astype(np.float32)
        frame = pd.DataFrame([row32.astype(float)], columns=cols)
        d2 = float(score_d2(model, frame)[0])
        near = bool(np.isfinite(d2) and abs(d2 - threshold) <= tolerance(threshold))
        out.append(
            {
                "name": name,
                "input": row32,
                "d2": d2,
                "alarm_row": bool(np.isfinite(d2) and d2 >= threshold),
                "near_threshold": near,
            }
        )
    return out


def persistence_sequence(threshold, k):
    """Scores (NaN = missing) and expected alarms for the C counter."""
    hi, lo = threshold * 1.5, threshold * 0.5
    seq = [lo] + [hi] * (k + 1) + [lo] + [hi] * (k - 1) + [math.nan] + [hi] * (k + 1)
    seq = np.asarray(seq, dtype=np.float32).astype(float)
    count, alarms = 0, []
    for value in seq:
        count = count + 1 if np.isfinite(value) and value >= threshold else 0
        alarms.append(count >= k)
    times = pd.date_range("2026-01-01T00:00:00+07:00", periods=len(seq), freq="s")
    persisted = persist(seq, times, times + pd.Timedelta(seconds=1), k)
    assert ((persisted >= threshold) == np.asarray(alarms)).all()
    return seq, alarms


def _c_float(value):
    value = np.float32(value)
    if np.isnan(value):
        return "NAN"
    if np.isinf(value):
        return "INFINITY" if value > 0 else "(-INFINITY)"
    text = f"{float(value):.9g}"
    if "e" not in text and "." not in text:
        text += ".0"
    return text + "f"


def _c_array(values):
    return "{" + ", ".join(_c_float(v) for v in values) + "}"


def c_header(manifest, set_name):
    model = manifest["model"]
    p = len(model["features"])
    golden = manifest["golden"]
    seq = manifest["persistence_sequence"]
    guard = f"AG_MAHAL_MODEL_{set_name.upper()}_H"
    numeric_index = [list(NUMERIC_COLUMNS).index(c) for c in model["features"]]
    lines = [
        f"/* Auto-generated by ml/baseline.py ({manifest['model_version']}) — do not edit.",
        f" * feature_set={set_name}  data={manifest['data_source']}",
        " * สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง เมื่อ data=synthetic",
        " *",
        " * x = one CSV §7 row, numeric columns only, in §7 order:",
        " *   0 pm1_0, 1 pm2_5, 2 pm10, 3 temp_c, 4 humidity_pct, 5 tvoc_ppb,",
        " *   6 eco2_ppm, 7 co_ppm   (CSV column = index + 1; NAN = empty field)",
        " * t_j = transform_j(x[AG_MAHAL_NUMERIC_INDEX[j]])",
        " *   IDENTITY: valid iff isfinite(x)",
        " *   LOG1P   : valid iff isfinite(x) && x >= 0, then log1pf(x)",
        " *   any invalid selected feature -> D2 missing (no imputation, no zero)",
        " * d_j = (t_j - MEAN[j]) / STD[j] - CENTER[j]",
        " * D2  = sum_i sum_j d_i * PRECISION[i][j] * d_j   (squared, no sqrt)",
        " * alarm iff D2 >= THRESHOLD_D2 on PERSISTENCE_K consecutive 1 s rows;",
        " *   a row below threshold, a missing D2 or a skipped sample resets.",
        " * Compare with golden: |D2_c - D2_ref| <= TOL_ABS + TOL_REL*|D2_ref|;",
        " *   skip the decision check where GOLDEN_NEAR_THRESHOLD = 1.",
        " * Normalization is frozen from training; never recompute on the board.",
        " */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <math.h>",
        "",
        f'#define AG_MAHAL_MODEL_VERSION "{manifest["model_version"]}"',
        f"#define AG_MAHAL_N_FEATURES {p}",
        f"#define AG_MAHAL_N_NUMERIC {len(NUMERIC_COLUMNS)}",
        "#define AG_MAHAL_TRANSFORM_IDENTITY 0",
        "#define AG_MAHAL_TRANSFORM_LOG1P 1",
        f"#define AG_MAHAL_THRESHOLD_D2 {_c_float(manifest['threshold_d2'])}",
        f"#define AG_MAHAL_PERSISTENCE_K {manifest['persistence_k']}",
        f"#define AG_MAHAL_TOL_ABS {_c_float(TOL_ABS)}",
        f"#define AG_MAHAL_TOL_REL {_c_float(TOL_REL)}",
        "",
        "static const char *const AG_MAHAL_FEATURE_NAMES[AG_MAHAL_N_FEATURES] = {"
        + ", ".join(f'"{c}"' for c in model["features"])
        + "};",
        "static const unsigned char AG_MAHAL_NUMERIC_INDEX[AG_MAHAL_N_FEATURES] = {"
        + ", ".join(str(i) for i in numeric_index)
        + "};",
        "static const unsigned char AG_MAHAL_TRANSFORM[AG_MAHAL_N_FEATURES] = {"
        + ", ".join(str(TRANSFORM_CODES[t]) for t in model["transforms"])
        + "};",
        f"static const float AG_MAHAL_MEAN[AG_MAHAL_N_FEATURES] = {_c_array(model['mean'])};",
        f"static const float AG_MAHAL_STD[AG_MAHAL_N_FEATURES] = {_c_array(model['std'])};",
        f"static const float AG_MAHAL_CENTER[AG_MAHAL_N_FEATURES] = {_c_array(model['center'])};",
        "static const float AG_MAHAL_PRECISION[AG_MAHAL_N_FEATURES][AG_MAHAL_N_FEATURES] = {",
        *[f"    {_c_array(row)}," for row in model["precision"]],
        "};",
        "",
        f"#define AG_MAHAL_N_GOLDEN {len(golden)}",
        "/* " + " | ".join(f"{i}:{g['name']}" for i, g in enumerate(golden)) + " */",
        "static const float AG_MAHAL_GOLDEN_INPUT[AG_MAHAL_N_GOLDEN][AG_MAHAL_N_NUMERIC] = {",
        *[f"    {_c_array(g['input'])}," for g in golden],
        "};",
        "/* NAN = D2 must be reported missing */",
        f"static const float AG_MAHAL_GOLDEN_D2[AG_MAHAL_N_GOLDEN] = {_c_array([g['d2'] for g in golden])};",
        "static const unsigned char AG_MAHAL_GOLDEN_ALARM_ROW[AG_MAHAL_N_GOLDEN] = {"
        + ", ".join(str(int(g["alarm_row"])) for g in golden)
        + "};",
        "static const unsigned char AG_MAHAL_GOLDEN_NEAR_THRESHOLD[AG_MAHAL_N_GOLDEN] = {"
        + ", ".join(str(int(g["near_threshold"])) for g in golden)
        + "};",
        "",
        "/* Persistence counter fixture: D2 per row (NAN = missing) -> alarm */",
        f"#define AG_MAHAL_N_SEQUENCE {len(seq['d2'])}",
        f"static const float AG_MAHAL_SEQUENCE_D2[AG_MAHAL_N_SEQUENCE] = {_c_array(seq['d2'])};",
        "static const unsigned char AG_MAHAL_SEQUENCE_ALARM[AG_MAHAL_N_SEQUENCE] = {"
        + ", ".join(str(int(a)) for a in seq["alarm"])
        + "};",
        "",
        f"#endif /* {guard} */",
        "",
    ]
    return "\n".join(lines)


def _jsonable(value):
    if isinstance(value, dict):
        return {k: _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, np.ndarray):
        return _jsonable(value.tolist())
    if isinstance(value, (np.floating, float)):
        value = float(value)
        return value if math.isfinite(value) else None
    if isinstance(value, (np.integer, np.bool_)):
        return value.item()
    return value


def build_manifest(result, set_name, data_source):
    model = result["model"]
    op = result["operating"]["mahalanobis"]
    golden = golden_vectors(
        model,
        op["threshold"],
        result["val"],
        result["val_scores"],
        result["val_events"],
        result["seed"],
    )
    seq, alarms = persistence_sequence(op["threshold"], op["k"])
    return {
        "model_version": f"{MODEL_VERSION}+{set_name}+{result['hash'][:12]}",
        "frozen_sha256": result["hash"],
        "data_source": data_source,
        "feature_set": set_name,
        "model": model,
        "score": "mahalanobis_d2_squared",
        "threshold_d2": op["threshold"],
        "persistence_k": op["k"],
        "missing_policy": "any selected feature non-finite (or log1p input < 0) "
        "-> score missing; no imputation; missing resets persistence",
        "tolerance": {"abs": TOL_ABS, "rel": TOL_REL},
        "golden": golden,
        "persistence_sequence": {"d2": seq, "alarm": alarms},
    }


def export(result, out, set_name, data_source):
    """Write model.json, norm.json, results.json, C header and test audit."""
    out = Path(out).resolve()
    allowed = Path(__file__).resolve().parents[1] / "data" / "processed"
    if not out.is_relative_to(allowed.resolve()):
        raise ValueError("ML-3 output must be under data/processed/")
    out.mkdir(parents=True, exist_ok=True)
    manifest = build_manifest(result, set_name, data_source)
    model = manifest["model"]
    payload = {
        **manifest,
        "model": {k: v for k, v in model.items() if k != "normalizer"},
        "golden": [
            {
                **g,
                "input": [None if not np.isfinite(v) else float(v) for v in g["input"]],
            }
            for g in manifest["golden"]
        ],
        "feature_numeric_index": [
            list(NUMERIC_COLUMNS).index(c) for c in model["features"]
        ],
        "selection": result["operating"],
        "fa_budget_per_day": result["budget"],
        "persistence_grid": result["grid"],
        "event_mapping": result["mapping"],
        "seed": result["seed"],
        "split": result["split"],
        "chi2_reference": result["chi2_reference"],
    }
    (out / "model.json").write_text(
        json.dumps(_jsonable(payload), ensure_ascii=False, indent=2, allow_nan=False)
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    save_normalizer(out / "norm.json", model["normalizer"])
    header = out / f"ag_mahal_{set_name}.h"
    header.write_text(c_header(manifest, set_name), encoding="utf-8", newline="\n")
    (out / "results.json").write_text(
        json.dumps(
            _jsonable({"frozen_sha256": result["hash"], "test": result["results"]}),
            ensure_ascii=False,
            indent=2,
            allow_nan=False,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    save_result(
        out / "test-mahalanobis", result["results"]["mahalanobis"], result["test_audit"]
    )
    return manifest, header


# ---------------------------------------------------------------------- CLI --
def _synth_frame(days, seed):
    raw, events = generate(days=days, seed=seed)
    raw = raw.set_index(pd.DatetimeIndex(pd.to_datetime(raw.pop("timestamp_iso"))))
    raw.index.name = "timestamp_iso"
    for column in ("start_iso", "end_iso"):
        events[column] = pd.to_datetime(events[column])
    return raw, events


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--feature-set",
        nargs="+",
        default=list(FEATURE_SETS),
        help="full, pm_th, or a comma list of §7 columns",
    )
    parser.add_argument("--raw", nargs="*", type=Path, help="real §7 CSV files")
    parser.add_argument("--events", type=Path, help="events CSV for --raw")
    parser.add_argument(
        "--mapping",
        default="",
        help="kind=normal|anomaly|unknown,... (required with --raw)",
    )
    parser.add_argument(
        "--row-support-s",
        type=float,
        default=None,
        help="observed support per row; required with --raw",
    )
    parser.add_argument("--days", type=float, default=14)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--t1", help="train/val boundary (+07:00)")
    parser.add_argument("--t2", help="val/test boundary (+07:00)")
    parser.add_argument("--gap-s", type=float, default=60)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    np.random.seed(args.seed)  # no global RNG is used; set for completeness
    if args.raw:
        if not (
            args.events and args.mapping and args.row_support_s and args.t1 and args.t2
        ):
            parser.error("--raw needs --events --mapping --row-support-s --t1 --t2")
        frame, events = load_raw(args.raw), load_events(args.events)
        mapping = dict(item.split("=", 1) for item in args.mapping.split(","))
        support_s, source = args.row_support_s, "real"
        t1, t2 = args.t1, args.t2
    else:
        frame, events = _synth_frame(args.days, args.seed)
        mapping, support_s, source = SYNTH_MAPPING, 1.0, "synthetic"
        start = frame.index[0]
        # 1 Hz continuous by construction → each row observed for 1 s.
        t1 = args.t1 or start + pd.Timedelta(days=int(args.days * 8 / 14))
        t2 = args.t2 or start + pd.Timedelta(days=int(args.days * 11 / 14))
    print(
        "สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง" if source == "synthetic" else "real data"
    )
    for spec in args.feature_set:
        name = spec if spec in FEATURE_SETS else "custom"
        features = spec if spec in FEATURE_SETS else spec.split(",")
        result = run(
            frame,
            events,
            features=features,
            t1=t1,
            t2=t2,
            gap_s=args.gap_s,
            support_s=support_s,
            mapping=mapping,
            seed=args.seed,
        )
        manifest, header = export(result, args.out / name, name, source)
        diag = result["model"]["diagnostics"]
        print(f"\n== {name}: {result['model']['features']}")
        print(f"version={manifest['model_version']}")
        print(
            f"cond(z) sample={diag['cond_sample_z']:.1f} "
            f"shrunk={diag['cond_shrunk_z']:.1f} "
            f"LW={diag['ledoit_wolf_shrinkage']:.3g} "
            f"raw-units={diag['cond_raw_units_transformed']:.3g}"
        )
        for method, op in result["operating"].items():
            print(f"{method}: threshold={op['threshold']:.6g} k={op['k']}")
        for method, summary in result["results"].items():
            print(
                f"test {method}: AP(raw)={summary['ap_raw_score_k1']} "
                f"AP(op)={summary['ap']} P={summary['precision']} "
                f"R={summary['recall']} detect={summary['detected_events']}/"
                f"{summary['event_count']} MTTD={summary['mttd_detected_s']} "
                f"FA/day={summary['false_alarm_episodes_per_day']}"
            )
        print(f"header={header}")


if __name__ == "__main__":
    main()
