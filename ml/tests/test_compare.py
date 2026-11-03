"""ML-3b smoke tests: the comparison runs every method on the ML-3 protocol and
is reproducible. Synthetic data only (สมมติฐานสำหรับข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง)."""

import pytest

from ml import baseline as b
from ml import compare as c

TIMING = {"score_s", "fit_all_s"}


@pytest.fixture(scope="module")
def small():
    # 4 days: train 0-1, val 2, test 3 (one event of each type in test)
    return b._synth_frame(4, 7)


def _run(small):
    frame, events = small
    return c.compare_one(frame, events, "pm_th", seed=7, split_days=(2, 3))


def test_every_method_runs_on_the_same_protocol(small):
    rows = _run(small)
    names = [r["method"] for r in rows]
    assert names == ["mahalanobis", "zmax", "pca_residual", "isolation_forest",
                     "lof", "autoencoder", "zmax_fixed_3sigma"]
    for r in rows:
        assert r["test_events"] == 3                      # decorrelation, spike, drift
        assert r["test_fa_per_day"] is not None           # cooking counts as normal time
        assert "accuracy" not in r                        # CLAUDE.md §2
    by = {r["method"]: r for r in rows}
    assert by["zmax_fixed_3sigma"]["threshold"] == 3.0 and by["zmax_fixed_3sigma"]["k"] == 1
    # the project's core claim on its own synthetic design
    assert by["mahalanobis"]["test_per_type"]["decorrelation"] == "1/1"
    assert by["zmax_fixed_3sigma"]["test_per_type"]["decorrelation"] == "0/1"


def test_reproducible_with_the_same_seed(small):
    strip = lambda rows: [{k: v for k, v in r.items() if k not in TIMING} for r in rows]
    assert strip(_run(small)) == strip(_run(small))


def test_output_must_stay_under_data_processed(tmp_path):
    with pytest.raises(SystemExit):
        c.main(["--seeds", "1", "--out", str(tmp_path / "x")])
