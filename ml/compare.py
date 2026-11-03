"""ML-3b: เทียบ Mahalanobis D² กับวิธี anomaly detection อื่นบนข้อมูลสังเคราะห์.

ข้อมูลทั้งหมดในโมดูลนี้ = สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง · ผลใช้ตอบว่า
"ภายใต้ anomaly 3 แบบที่ออกแบบไว้ (ml/synth.py) วิธีไหนแยกได้ดีกว่ากัน" เท่านั้น
ไม่ใช่หลักฐานว่าวิธีใดดีกว่าในห้องจริง · ไม่แตะข้อมูลจริงหรือ test ของ R4

    python -m ml.compare --seeds 1 2 3 4 5 --feature-set pm_th full --out data/processed/compare

ทุกวิธีใช้ protocol เดียวกับ ml/baseline.py (ML-3) จึงเทียบกันได้:
1. ข้อมูลเดียวกัน · แบ่งตามเวลาเดียวกัน (train วัน 0–7 · val 8–10 · test 11–13)
2. transform + normalizer เดียวกัน (fit บน train แถว normal เท่านั้น) → ทุกวิธีเห็น z ชุดเดียวกัน
3. ทุกวิธี fit บน train แถว normal ที่ครบทุกฟีเจอร์เท่านั้น (unsupervised · ไม่เห็น anomaly)
4. threshold + k เลือกบน val ด้วย select_operating_point ตัวเดียวกัน (งบเตือนผิด ≤ 1/วัน)
5. test ให้คะแนนครั้งเดียวต่อวิธี ด้วย ml/metrics.py (ml2-v1 · ไม่มี accuracy · ไม่มี PA)
6. ทำอาหารสังเคราะห์ไม่ได้ annotate → นับเป็นช่วงปกติ: เตือนตอนทำอาหาร = เตือนผิด

ทุกวิธีให้คะแนน "ยิ่งสูงยิ่งผิดปกติ" · แถวที่ฟีเจอร์ไม่ครบ = NaN (ไม่ impute) เหมือน D²
"""

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.decomposition import PCA
from sklearn.ensemble import IsolationForest
from sklearn.neighbors import LocalOutlierFactor
from sklearn.neural_network import MLPRegressor

try:
    from . import baseline as b
    from .split import chronological_split, normal_only
    from .synth import SynthConfig
except ImportError:  # `python ml/compare.py`
    import baseline as b
    from split import chronological_split, normal_only
    from synth import SynthConfig

COMPARE_VERSION = "ml3b-v1"
# Subsample for the slow learners (LOF neighbours, autoencoder epochs). Train
# normal has ~690k complete rows; 20k–50k already cover the daily cycle many
# times over. Fixed by seed, so the run is reproducible.
LOF_FIT_ROWS = 20000
AE_FIT_ROWS = 50000
PCA_VARIANCE = 0.90   # keep components up to 90 % of train variance


def _complete(z):
    return np.isfinite(z).all(axis=1)


def _score_rows(z, fn):
    """Apply fn to complete rows; NaN elsewhere (same missing policy as D²)."""
    out = np.full(len(z), np.nan)
    ok = _complete(z)
    if ok.any():
        out[ok] = fn(z[ok])
    return out


def _subsample(z, rows, seed):
    if len(z) <= rows:
        return z
    rng = np.random.default_rng(seed)
    return z[np.sort(rng.choice(len(z), size=rows, replace=False))]


def build_methods(model, train_z, seed):
    """Fit every comparison method on the same train-normal z rows.

    Returns {name: (score_fn(z) -> scores, info dict)}. Parameter counts are a
    rough size for the board (floats to store), not a measured footprint.
    """
    p = train_z.shape[1]
    center = model["center"].astype(float)
    precision = model["precision"].astype(float)
    methods = {}

    methods["mahalanobis"] = (
        lambda z: np.einsum("ni,ij,nj->n", z - center, precision, z - center),
        {"params": p * p + 3 * p, "note": "D² = dᵀΣ⁻¹d (ml/baseline.py)"},
    )
    methods["zmax"] = (
        lambda z: np.abs(z).max(axis=1),
        {"params": 2 * p, "note": "max |z| ทีละตัว · เกณฑ์เลือกบน val"},
    )

    pca = PCA(n_components=PCA_VARIANCE, svd_solver="full", random_state=seed)
    pca.fit(train_z)
    q = int(pca.n_components_)

    def pca_residual(z):
        recon = pca.inverse_transform(pca.transform(z))
        return ((z - recon) ** 2).sum(axis=1)

    methods["pca_residual"] = (
        pca_residual,
        {"params": p * q + p, "note": f"ระยะจากระนาบ {q} แกนหลัก (SPE/Q) · {PCA_VARIANCE:.0%} variance"},
    )

    forest = IsolationForest(n_estimators=200, max_samples=256, random_state=seed)
    forest.fit(train_z)
    nodes = int(sum(t.tree_.node_count for t in forest.estimators_))
    methods["isolation_forest"] = (
        lambda z: -forest.score_samples(z),
        {"params": nodes * 2, "note": "200 trees · max_samples 256"},
    )

    lof = LocalOutlierFactor(n_neighbors=35, novelty=True)
    lof_rows = _subsample(train_z, LOF_FIT_ROWS, seed)
    lof.fit(lof_rows)
    methods["lof"] = (
        lambda z: -lof.score_samples(z),
        {"params": int(lof_rows.size), "note": f"k=35 · เก็บ {len(lof_rows)} แถว train"},
    )

    # Autoencoder: p -> 8 -> (p-1) -> 8 -> p, tanh. The bottleneck is one
    # narrower than the input, so it must learn the relation between the
    # features (an identity map is impossible).
    ae = MLPRegressor(
        hidden_layer_sizes=(8, max(p - 1, 1), 8),
        activation="tanh",
        max_iter=200,
        early_stopping=True,
        random_state=seed,
    )
    ae_rows = _subsample(train_z, AE_FIT_ROWS, seed + 1)
    ae.fit(ae_rows, ae_rows)
    weights = int(sum(w.size for w in ae.coefs_) + sum(c.size for c in ae.intercepts_))
    methods["autoencoder"] = (
        lambda z: ((z - ae.predict(z)) ** 2).mean(axis=1),
        {"params": weights, "note": f"MLP {p}-8-{max(p - 1, 1)}-8-{p} tanh · {len(ae_rows)} แถว"},
    )
    return methods


def _cooking_windows(start, days):
    """Synthetic cooking intervals (SynthConfig schedule, not annotated in events)."""
    cfg = SynthConfig()
    return [
        (start + pd.Timedelta(days=d, seconds=o),
         start + pd.Timedelta(days=d, seconds=o + cfg.cooking_duration_s))
        for d in range(days) for o in cfg.cooking_offsets_s
    ]


def _fa_in_cooking(summary, windows):
    """False-alarm episodes whose onset falls inside a cooking interval: the
    project's claim is that PM+T+RH rising together is normal (CLAUDE.md §1)."""
    count = 0
    for ep in summary.get("alarm_episodes", []):
        if ep["onset_truth"] != "normal":
            continue
        t = pd.Timestamp(ep["start_iso"])
        count += any(lo <= t < hi for lo, hi in windows)
    return count


def _per_type(summary):
    counts = {}
    for event in summary.get("events", []):
        hit, total = counts.get(event["label"], (0, 0))
        counts[event["label"]] = (hit + int(event["detected"]), total + 1)
    return {label: f"{hit}/{total}" for label, (hit, total) in sorted(counts.items())}


def compare_one(frame, events, features, seed, gap_s=60.0, support_s=1.0,
                split_days=(8, 11)):
    """One seed, one feature set: fit → select on val → test once per method.

    split_days: (t1, t2) in days from the first row; (8, 11) = the ML-3
    split of 14 synthetic days (ml/baseline.py main)."""
    start = frame.index[0]
    t1 = start + pd.Timedelta(days=split_days[0])
    t2 = start + pd.Timedelta(days=split_days[1])
    train, val, test = chronological_split(frame, t1, t2, gap_s)
    b.check_event_boundaries(events, t1, t2, gap_s)
    model = b.fit(train, features)
    train_z = b._z(model, normal_only(train))
    train_z = train_z[_complete(train_z)]
    support = pd.Timedelta(seconds=support_s)
    val_ends, test_ends = val.index + support, test.index + support
    val_events = b.split_events(events, val, val_ends)
    test_events = b.split_events(events, test, test_ends)
    val_z, test_z = b._z(model, val), b._z(model, test)
    mapping = b.SYNTH_MAPPING
    cooking = _cooking_windows(start, int(np.ceil((frame.index[-1] - start) / pd.Timedelta(days=1))))

    rows = []
    t_fit = time.perf_counter()
    methods = build_methods(model, train_z, seed)
    fit_s = time.perf_counter() - t_fit
    plans = [(name, fn, info, "validation") for name, (fn, info) in methods.items()]
    plans.append(("zmax_fixed_3sigma", methods["zmax"][0], {"params": 6, "note": "|z| ≥ 3 · k = 1"}, "fixed"))
    for name, fn, info, source in plans:
        t0 = time.perf_counter()
        val_raw = _score_rows(val_z, fn)
        test_raw = _score_rows(test_z, fn)
        score_s = time.perf_counter() - t0
        if source == "validation":
            op = b.select_operating_point(val, val_events, val_raw, val_ends, mapping)
            threshold, k = op["threshold"], op["k"]
        else:
            threshold, k = b.CLASSIC_SIGMA, 1
        scores = b.persist(test_raw, test.index, test_ends, k)
        summary, _ = b.run_metrics(test, test_events, scores, test_ends, mapping, threshold, source)
        ranking, _ = b.run_metrics(test, test_events, test_raw, test_ends, mapping, threshold, source)
        rows.append(
            {
                "seed": seed,
                "feature_set": features,
                "method": name,
                "threshold": float(threshold),
                "k": int(k),
                "test_detected": summary["detected_events"],
                "test_events": summary["event_count"],
                "test_detection_rate": summary["event_detection_rate"],
                "test_per_type": _per_type(summary),
                "test_mttd_s": summary["mttd_detected_s"],
                "test_fa_episodes": summary["false_alarm_episodes"],
                "test_fa_per_day": summary["false_alarm_episodes_per_day"],
                "test_fa_in_cooking": _fa_in_cooking(summary, cooking),
                "test_ap_raw": ranking["ap"],
                "params": info["params"],
                "note": info["note"],
                "score_s": round(score_s, 2),
            }
        )
    for row in rows:
        row["fit_all_s"] = round(fit_s, 1)
    return rows


def render_markdown(rows):
    df = pd.DataFrame(rows)
    lines = [
        f"# ML-3b เทียบวิธี (`{COMPARE_VERSION}`) — สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง",
        "",
        "test = 3 วัน × (decorrelation · spike · drift) ต่อ seed · ทำอาหาร = ช่วงปกติ (เตือน = เตือนผิด)",
        "",
    ]
    for fs, part in df.groupby("feature_set", sort=False):
        seeds = sorted(int(s) for s in part.seed.unique())
        lines += [f"## {fs} · seeds {seeds}", "",
                  "| วิธี | detect รวม | decorrelation | spike | drift | FA/วัน เฉลี่ย (ช่วง) | FA ตอนทำอาหาร / FA ทั้งหมด | MTTD s เฉลี่ย | AP(raw) เฉลี่ย | พารามิเตอร์ |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for method, g in part.groupby("method", sort=False):
            def tot(label):
                hit = tot_n = 0
                for d in g.test_per_type:
                    if label in d:
                        h, n = d[label].split("/")
                        hit, tot_n = hit + int(h), tot_n + int(n)
                return f"{hit}/{tot_n}" if tot_n else "—"
            fa = g.test_fa_per_day.astype(float)
            mttd = g.test_mttd_s.dropna().astype(float)
            ap = g.test_ap_raw.dropna().astype(float)
            lines.append(
                f"| {method} | {int(g.test_detected.sum())}/{int(g.test_events.sum())} | "
                f"{tot('decorrelation')} | {tot('spike')} | {tot('drift')} | "
                f"{fa.mean():.2f} ({fa.min():.2f}–{fa.max():.2f}) | "
                f"{int(g.test_fa_in_cooking.sum())}/{int(g.test_fa_episodes.sum())} | "
                f"{(mttd.mean() if len(mttd) else float('nan')):.0f} | "
                f"{(ap.mean() if len(ap) else float('nan')):.3f} | {int(g.params.median())} |"
            )
        lines.append("")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 4, 5])
    parser.add_argument("--feature-set", nargs="+", default=["pm_th", "full"])
    parser.add_argument("--days", type=int, default=14)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    out = args.out.resolve()
    allowed = (Path(__file__).resolve().parents[1] / "data" / "processed").resolve()
    if not out.is_relative_to(allowed):
        raise SystemExit("output must be under data/processed/")
    rows = []
    for seed in args.seeds:
        np.random.seed(seed)   # sklearn objects get random_state explicitly too
        frame, events = b._synth_frame(args.days, seed)
        for fs in args.feature_set:
            t0 = time.perf_counter()
            rows += compare_one(frame, events, fs, seed)
            print(f"seed {seed} {fs}: {time.perf_counter() - t0:.0f} s", flush=True)
    out.mkdir(parents=True, exist_ok=True)
    (out / "compare.json").write_text(
        json.dumps({"version": COMPARE_VERSION, "data": "synthetic", "rows": rows},
                   ensure_ascii=False, indent=2, default=float) + "\n",
        encoding="utf-8", newline="\n")
    text = render_markdown(rows)
    (out / "compare.md").write_text(text + "\n", encoding="utf-8", newline="\n")
    print(text)


if __name__ == "__main__":
    main()
