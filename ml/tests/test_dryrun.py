"""R2: ซ้อมทั้งสาย USB → QA → events → baseline → header (ข้อมูลสังเคราะห์ ไม่ใช่ค่าวัดจริง)"""

import json
import shutil

import pytest

from ml import baseline, dryrun, events, qa

T1 = "2026-09-25T00:00:00+07:00"
T2 = "2026-09-26T00:00:00+07:00"
MAPPING = "spike=anomaly,drift=anomaly,decorrelation=anomaly"


def test_end_to_end_dry_run(output_dir):
    info = dryrun.make(output_dir, days=4, seed=42)
    usb = output_dir / "usb"
    raw_glob = str(usb / "AG_*_usb.csv")
    ev_glob = str(usb / "EV_*_usb.csv")

    report = qa.run([raw_glob], output_dir / "qa", make_plot=False)
    gaps = report["summary"]["gaps"]
    assert [g["kind"] for g in gaps] == [
        "transit",
        "reboot",
        "transit",
        "reboot",
        "transit",
    ]
    # รีบูตครั้งที่สอง PM มีค่าทันที (SNTP > 30 s) — ต้องจับได้จาก ENS160/CO
    assert gaps[3]["blank_after_s"]["pm2_5"] == 0
    for column in ("pm2_5", "temp_c", "humidity_pct", "co_ppm", "tvoc_ppb"):
        assert (
            report["summary"]["blank"][column]["longest_blank_run_outside_warmup_rows"]
            < 5
        )

    draft = output_dir / "draft.csv"
    n = events.draft([ev_glob], draft)
    filled = output_dir / "events.csv"
    assert dryrun.fill(draft, output_dir / "truth_events.csv", filled) == n == 12
    result = events.check(
        filled,
        [raw_glob],
        [ev_glob],
        allowed=("spike", "drift", "decorrelation"),
        t1=T1,
        t2=T2,
    )
    assert result["errors"] == [] and result["warnings"] == []

    files = [str(p) for p in sorted(usb.glob("AG_*_usb.csv"))]
    baseline.main(
        [
            "--raw",
            *files,
            "--events",
            str(filled),
            "--mapping",
            MAPPING,
            "--row-support-s",
            "1",
            "--t1",
            T1,
            "--t2",
            T2,
            "--feature-set",
            "pm_th",
            "--out",
            str(output_dir / "ml"),
        ]
    )
    model = json.loads((output_dir / "ml" / "pm_th" / "model.json").read_text("utf-8"))
    assert model["model"]["features"] == ["pm2_5", "temp_c", "humidity_pct"]
    assert info["start"] == "2026-09-23T00:00:00+07:00"
    if shutil.which("gcc") is None:
        pytest.skip("gcc not on PATH")
    ok, last = dryrun.check_header(output_dir / "ml" / "pm_th" / "ag_mahal_pm_th.h")
    assert ok, last


def test_fill_refuses_to_overwrite(output_dir):
    target = output_dir / "events.csv"
    target.write_text("keep\n", encoding="utf-8")
    with pytest.raises(ValueError, match="ไม่เขียนทับ"):
        dryrun.fill(target, target, target)
    assert target.read_text(encoding="utf-8") == "keep\n"
