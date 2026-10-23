import pytest

from ml.events import check, draft, main
from ml.qa import QAInputError
from ml.tests.test_qa import make_rows, write

EV_HEADER = "start_iso,end_iso,label,note\n"
# เฟิร์มแวร์: start = แถว marked แรก · end = แถว marked สุดท้าย (ไม่ +1 s)
EV_ROWS = (
    "2026-10-24T10:01:40+07:00,2026-10-24T10:02:39+07:00,marked,\n"
    "2026-10-24T10:03:20+07:00,2026-10-24T10:03:49+07:00,marked,\n"
)
MARKED = {
    **{s: "marked" for s in range(100, 160)},
    **{s: "marked" for s in range(200, 230)},
}
NAMED = (
    EV_HEADER
    + "2026-10-24T10:01:40+07:00,2026-10-24T10:02:40+07:00,incense,ธูป 1 ดอก 1 m\n"
    "2026-10-24T10:03:20+07:00,2026-10-24T10:03:50+07:00,dusting,สะบัดผ้า 30 cm\n"
)


@pytest.fixture
def files(output_dir):
    return {
        "raw": write(output_dir / "AG_1_usb.csv", make_rows(range(300), label=MARKED)),
        "ev": write(output_dir / "EV_1_usb.csv", EV_HEADER + EV_ROWS),
        "dir": output_dir,
    }


def test_draft_adds_one_second_and_todo_label(files):
    out = files["dir"] / "events" / "draft.csv"
    assert draft([files["ev"]], out) == 2
    lines = out.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "start_iso,end_iso,label,note"
    assert lines[1] == "2026-10-24T10:01:40+07:00,2026-10-24T10:02:40+07:00,TODO,"


def test_draft_never_overwrites(files):
    out = write(files["dir"] / "existing.csv", NAMED)
    with pytest.raises(QAInputError, match="ไม่เขียนทับ"):
        draft([files["ev"]], out)
    assert out.read_text(encoding="utf-8") == NAMED


def test_unedited_draft_fails_check(files):
    out = files["dir"] / "draft.csv"
    draft([files["ev"]], out)
    result = check(out, [files["raw"]], [files["ev"]])
    assert any("label ไม่อยู่ใน" in e and "TODO" in e for e in result["errors"])
    assert any("note ว่าง" in e for e in result["errors"])


def test_named_events_pass_against_raw_and_ev(files):
    events = write(files["dir"] / "named.csv", NAMED)
    result = check(events, [files["raw"]], [files["ev"]])
    assert result["errors"] == [] and result["warnings"] == []
    assert result["by_label"] == {"dusting": 1, "incense": 1}


def test_mark_missing_from_events_is_an_error(files):
    events = write(
        files["dir"] / "one.csv",
        NAMED.splitlines(keepends=True)[0] + NAMED.splitlines(keepends=True)[1],
    )
    result = check(events, [files["raw"]], [files["ev"]])
    assert any("MARK ใน EV ยังไม่ได้ลง" in e for e in result["errors"])
    assert any("ช่วง marked ใน raw ไม่มีใน events" in e for e in result["errors"])


def test_manual_event_is_a_warning_not_error(files):
    events = write(
        files["dir"] / "manual.csv",
        NAMED
        + "2026-10-24T10:04:30+07:00,2026-10-24T10:04:50+07:00,cooking,ผัดผัก จดมือ\n",
    )
    result = check(events, [files["raw"]], [files["ev"]])
    assert result["errors"] == []
    assert len(result["manual"]) == 1 and "cooking" in result["manual"][0]


def test_overlap_and_unknown_label_are_errors(files):
    events = write(
        files["dir"] / "bad.csv",
        EV_HEADER + "2026-10-24T10:00:00+07:00,2026-10-24T10:05:00+07:00,incense,a\n"
        "2026-10-24T10:04:00+07:00,2026-10-24T10:06:00+07:00,spray,b\n",
    )
    errors = check(events)["errors"]
    assert any("ซ้อนกัน" in e for e in errors)
    assert any("spray" in e for e in errors)


def test_event_crossing_split_boundary_is_an_error(files):
    events = write(files["dir"] / "named.csv", NAMED)
    t1 = "2026-10-24T10:02:00+07:00"  # อยู่กลางเหตุการณ์ incense
    t2 = "2026-10-24T12:00:00+07:00"
    errors = check(events, t1=t1, t2=t2)["errors"]
    assert any("คร่อมขอบ" in e for e in errors)
    ok = check(events, t1="2026-10-24T09:00:00+07:00", t2=t2, gap_s=60)
    assert ok["errors"] == []


def test_cli_exit_codes(files):
    named = write(files["dir"] / "named.csv", NAMED)
    args = ["check", "--events", str(named), "--raw", str(files["raw"]), "--ev"]
    assert main([*args, str(files["dir"] / "EV_*_usb.csv")]) == 0
    draft_out = files["dir"] / "d.csv"
    assert main(["draft", "--ev", str(files["ev"]), "--out", str(draft_out)]) == 0
    assert main(["check", "--events", str(draft_out)]) == 1
    assert main(["check", "--events", str(files["dir"] / "missing.csv")]) == 2
