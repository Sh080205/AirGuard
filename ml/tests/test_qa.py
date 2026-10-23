from datetime import datetime, timedelta, timezone

import pytest

from ml.load import load_raw
from ml.qa import QAConfig, analyze, main, run, sha256

HEADER = (
    "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,tvoc_ppb,eco2_ppm,co_ppm,label"
)
TZ = timezone(timedelta(hours=7))
T0 = datetime(2026, 10, 24, 10, 0, 0, tzinfo=TZ)


def make_rows(seconds, *, pm=None, temp=None, label=None, blank=None):
    """แถว §7 ที่ค่าเปลี่ยนทุกแถว (ไม่ค้าง) · seconds = วินาทีนับจาก T0

    pm/temp/label/blank: dict วินาที → ค่าแทน · blank: dict วินาที → ชุดคอลัมน์ที่ว่าง
    """
    lines = [HEADER]
    for s in seconds:
        stamp = (T0 + timedelta(seconds=s)).isoformat()
        fields = {
            "pm1_0": str(5 + s % 7),
            "pm2_5": str((pm or {}).get(s, 8 + s % 5)),
            "pm10": str(12 + s % 9),
            "temp_c": f"{(temp or {}).get(s, 30.0 + (s % 10) / 10):.1f}",
            "humidity_pct": f"{55.0 + (s % 8) / 10:.1f}",
            "tvoc_ppb": str(100 + s % 11),
            "eco2_ppm": str(500 + s % 13),
            "co_ppm": f"{(s % 3) / 10:.1f}",
        }
        for column in (blank or {}).get(s, ()):
            fields[column] = ""
        row = [stamp, *fields.values(), (label or {}).get(s, "normal")]
        lines.append(",".join(row))
    return "\n".join(lines) + "\n"


def write(path, text):
    path.write_text(text, encoding="utf-8")
    return path


def qa(output_dir, text, **kwargs):
    return analyze(load_raw(write(output_dir / "AG_x_usb.csv", text)), **kwargs)


def test_one_second_step_is_not_a_gap_two_seconds_is(output_dir):
    clean = qa(output_dir, make_rows(range(100)))
    assert clean["gaps"] == [] and clean["missing_rows"] == 0
    s = qa(output_dir, make_rows([*range(50), *range(51, 100)]))
    assert len(s["gaps"]) == 1
    assert s["gaps"][0]["missing_rows"] == 1 and s["gaps"][0]["kind"] == "transit"
    assert s["missing_rows"] == 1


def test_reboot_is_separated_from_rows_lost_in_transit(output_dir):
    # รีบูต: ช่องว่าง 20 s แล้ว PM ว่าง 30 s (เฟิร์มแวร์ทิ้งเฟรมช่วงพัดลมอุ่นเครื่อง)
    after = list(range(120, 400))
    blank = {s: ("pm1_0", "pm2_5", "pm10") for s in range(120, 150)}
    # หายระหว่างทาง: ช่องว่าง 5 s แต่ PM มีค่าทันที
    seconds = [*range(100), *after[:200], *range(325, 400)]
    s = qa(output_dir, make_rows(seconds, blank=blank))
    kinds = [g["kind"] for g in s["gaps"]]
    assert kinds == ["reboot", "transit"]
    assert s["gaps"][0]["pm_blank_after_s"] == 30
    # PM ว่าง 30 แถวหลังรีบูตอยู่ในช่วงอุ่นเครื่องทั้งหมด
    assert s["blank"]["pm2_5"]["blank_rows"] == 30
    assert s["blank"]["pm2_5"]["blank_outside_warmup_rows"] == 0


def test_blank_outside_warmup_is_counted(output_dir):
    blank = {s: ("pm2_5",) for s in range(500, 520)}
    s = qa(output_dir, make_rows(range(1000), blank=blank))
    assert s["blank"]["pm2_5"]["blank_outside_warmup_rows"] == 20
    assert s["blank"]["pm2_5"]["longest_blank_run_outside_warmup_rows"] == 20
    assert s["blank"]["pm2_5"]["blank_outside_warmup_pct"] == 2.0
    assert s["verdict"] == "FAIL"


def test_aht21_repeat_every_two_rows_is_not_stuck(output_dir):
    temp = {s: 30.0 + (s // 2 % 10) / 10 for s in range(1200)}  # ซ้ำทุก 2 แถวแบบ AHT21
    s = qa(output_dir, make_rows(range(1200), temp=temp))
    assert s["stuck"] == []


@pytest.mark.parametrize("minutes, flagged", [(11, True), (9, False)])
def test_stuck_value_flagged_at_ten_minutes(output_dir, minutes, flagged):
    n = minutes * 60 + 1
    temp = {s: 31.0 for s in range(n)}
    s = qa(output_dir, make_rows(range(n + 60), temp=temp))
    stuck = [x for x in s["stuck"] if x["column"] == "temp_c"]
    assert bool(stuck) is flagged
    if flagged:
        assert stuck[0]["duration_s"] == minutes * 60 and stuck[0]["value"] == 31.0
        assert s["verdict"] == "WARN"


def test_blank_rows_do_not_split_a_stuck_run(output_dir):
    temp = {s: 31.0 for s in range(700)}
    blank = {s: ("temp_c",) for s in (200, 400, 401)}
    s = qa(output_dir, make_rows(range(760), temp=temp, blank=blank))
    stuck = [x for x in s["stuck"] if x["column"] == "temp_c"]
    assert len(stuck) == 1 and stuck[0]["duration_s"] == 699


def test_stuck_outside_pm_th_is_reported_but_not_warned(output_dir):
    text = make_rows(range(900))
    # co_ppm ความละเอียด 0.1 ppm นิ่งได้นานในอากาศสะอาด
    lines = text.splitlines()
    for k in range(1, len(lines)):
        fields = lines[k].split(",")
        fields[8] = "0.0"
        lines[k] = ",".join(fields)
    s = qa(output_dir, "\n".join(lines) + "\n")
    assert [x["column"] for x in s["stuck"]] == ["co_ppm"]
    assert s["verdict"] == "PASS"


def test_marked_without_event_is_reported(output_dir):
    label = {s: "marked" for s in range(100, 160)}
    raw = write(output_dir / "AG_x_usb.csv", make_rows(range(300), label=label))
    events = write(
        output_dir / "events.csv",
        "start_iso,end_iso,label,note\n"
        "2026-10-24T10:01:41+07:00,2026-10-24T10:02:41+07:00,incense,ธูป 1 ดอก\n"
        "2026-10-24T10:04:00+07:00,2026-10-24T10:04:30+07:00,cooking,จดด้วยมือ\n",
    )
    alone = analyze(load_raw(raw))
    assert alone["marked"][0]["event_label"] is None
    assert any("unknown" in r for r in alone["reasons"])
    report = run([raw], output_dir / "qa", events, make_plot=False)
    s = report["summary"]
    # ต้น/ปลายห่าง 1 s ≤ match_tol_s = 2 s → จับคู่ได้
    assert s["marked"] == [
        {
            "start": "2026-10-24T10:01:40+07:00",
            "end": "2026-10-24T10:02:40+07:00",
            "rows": 60,
            "event_label": "incense",
        }
    ]
    assert [e["label"] for e in s["events_without_marked"]] == ["cooking"]


def test_out_of_bounds_is_flagged_not_dropped(output_dir):
    text = make_rows(range(60)).replace(",55.3,", ",101.0,", 1)
    s = qa(output_dir, text)
    assert s["out_of_bounds"]["humidity_pct"]["rows"] == 1
    assert s["rows"] == 60


def test_raw_unchanged_and_rerun_is_byte_identical(output_dir):
    raw = write(output_dir / "AG_x_usb.csv", make_rows([*range(300), *range(305, 400)]))
    before = sha256(raw)
    run([raw], output_dir / "a", make_plot=False)
    run([raw], output_dir / "b", make_plot=False)
    assert sha256(raw) == before
    first = (output_dir / "a" / "qa.json").read_bytes()
    assert first == (output_dir / "b" / "qa.json").read_bytes()
    assert before.encode() in first
    assert (output_dir / "a" / "qa.md").read_text(encoding="utf-8").startswith("# QA")


def test_missing_rows_over_one_percent_fails(output_dir):
    s = qa(output_dir, make_rows([*range(100), *range(103, 200)]))
    assert s["missing_rows_pct"] == 1.5
    assert s["verdict"] == "FAIL"
    assert qa(output_dir, make_rows(range(200)))["verdict"] == "PASS"


def test_long_single_gap_fails_even_when_rare(output_dir):
    cfg = QAConfig(max_missing_rows_pct=100.0)
    s = qa(output_dir, make_rows([*range(100), *range(800, 900)]), cfg=cfg)
    assert s["longest_gap_s"] == 701 and s["gaps"][0]["missing_rows"] == 700
    assert s["verdict"] == "FAIL"


def test_glob_is_expanded_and_files_combined(output_dir):
    write(output_dir / "AG_1_usb.csv", make_rows(range(50)))
    write(output_dir / "AG_2_usb.csv", make_rows(range(50, 100)))
    report = run([str(output_dir / "AG_*_usb.csv")], output_dir / "qa", make_plot=False)
    assert [f["name"] for f in report["inputs"]] == ["AG_1_usb.csv", "AG_2_usb.csv"]
    assert report["summary"]["rows"] == 100


def test_bad_file_exits_2_and_names_it(output_dir, capsys):
    good = write(output_dir / "AG_1_usb.csv", make_rows(range(10)))
    bad = write(
        output_dir / "AG_2_usb.csv", make_rows(range(10, 20)).replace("label", "lbl")
    )
    code = main(
        ["--raw", str(good), str(bad), "--out", str(output_dir / "qa"), "--no-plot"]
    )
    assert code == 2
    assert "AG_2_usb.csv" in capsys.readouterr().err


def test_duplicate_timestamps_across_files_exit_2(output_dir, capsys):
    a = write(output_dir / "AG_1_usb.csv", make_rows(range(10)))
    b = write(output_dir / "AG_1_sd.csv", make_rows(range(5, 15)))
    code = main(["--raw", str(a), str(b), "--out", str(output_dir / "qa"), "--no-plot"])
    assert code == 2
    assert "duplicate" in capsys.readouterr().err


def test_exit_code_follows_verdict(output_dir):
    ok = write(output_dir / "AG_ok_usb.csv", make_rows(range(200)))
    assert main(["--raw", str(ok), "--out", str(output_dir / "q1"), "--no-plot"]) == 0
    bad = write(
        output_dir / "AG_bad_usb.csv", make_rows([*range(100), *range(110, 200)])
    )
    assert main(["--raw", str(bad), "--out", str(output_dir / "q2"), "--no-plot"]) == 1
