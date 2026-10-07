import pytest

from ml.load import load_events, load_raw

HEADER = (
    "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,tvoc_ppb,eco2_ppm,co_ppm,label"
)
ROW = "2026-09-23T00:00:00+07:00,8,12,15,28.4,62.1,145,520,0.4,normal"


def write(path, content):
    path.write_text(content + "\n", encoding="utf-8")
    return path


def test_missing_and_real_zero_are_distinct(output_dir):
    path = write(
        output_dir / "input.csv", HEADER + "\n" + ROW.replace(",8,12,15,", ",,0,15,")
    )
    df = load_raw(path)
    assert df.pm1_0.isna().all()
    assert df.pm2_5.iloc[0] == 0
    assert str(df.index.tz) == "UTC+07:00"


@pytest.mark.parametrize(
    "header",
    [
        HEADER.replace("pm2_5", "pm25"),
        HEADER + ",extra",
        HEADER.replace(",label", ""),
        HEADER.replace("pm1_0,pm2_5", "pm2_5,pm1_0"),
        HEADER.replace("pm2_5", "pm1_0"),
    ],
)
def test_strict_header(output_dir, header):
    path = write(output_dir / "bad.csv", header + "\n" + ROW)
    with pytest.raises(ValueError, match="header mismatch.*pm"):
        load_raw(path)


@pytest.mark.parametrize(
    "stamp",
    [
        "2026-09-23T00:00:00",
        "2026-09-22T17:00:00Z",
        "2026-09-23T00:00:00+08:00",
        "not-a-date",
        "2026-09-23 00:00:00+07:00",
        "",
    ],
)
def test_strict_timezone(output_dir, stamp):
    row = stamp + ROW[ROW.index(",") :]
    with pytest.raises(ValueError, match=r"timestamp_iso.*\+07:00"):
        load_raw(write(output_dir / "bad.csv", HEADER + "\n" + row))


def test_sort_multiple_files_and_detect_duplicate(output_dir):
    early = write(output_dir / "early.csv", HEADER + "\n" + ROW)
    late = write(
        output_dir / "late.csv", HEADER + "\n" + ROW.replace("00:00:00", "00:00:01")
    )
    frame = load_raw([late, early])
    assert frame.index.is_monotonic_increasing
    assert frame.index[0].second == 0
    with pytest.raises(ValueError, match="duplicate"):
        load_raw([early, early])
    duplicate = write(output_dir / "dup.csv", HEADER + "\n" + ROW + "\n" + ROW)
    with pytest.raises(ValueError, match="duplicate"):
        load_raw(duplicate)


@pytest.mark.parametrize(
    "row",
    [
        ROW + ",extra",
        ROW.replace(",normal", ""),
        ROW.replace("normal", "spike"),
        ROW.replace(",8,", ",bad,"),
        ROW.replace(",8,", ",8.5,"),
        ROW.replace(",8,", ",NaN,"),
        ROW.replace(",8,", ",inf,"),
    ],
)
def test_invalid_raw_fields(output_dir, row):
    with pytest.raises(ValueError):
        load_raw(write(output_dir / "bad.csv", HEADER + "\n" + row))


def test_empty_raw_header_only(output_dir):
    assert load_raw(write(output_dir / "empty.csv", HEADER)).empty
    with pytest.raises(ValueError, match="at least one"):
        load_raw([])


def test_events_schema_notes_and_half_open_times(output_dir):
    content = (
        "start_iso,end_iso,label,note\n"
        '2026-09-23T00:01:00+07:00,2026-09-23T00:02:00+07:00,incense,"one, two"\n'
        "2026-09-23T00:00:00+07:00,2026-09-23T00:01:00+07:00,decorrelation,"
    )
    events = load_events(write(output_dir / "events.csv", content))
    assert events.label.tolist() == ["decorrelation", "incense"]
    assert events.note.tolist() == ["", "one, two"]
    assert str(events.start_iso.dt.tz) == "UTC+07:00"


@pytest.mark.parametrize(
    "content",
    [
        "start_iso,end_iso,label,wrong",
        "start_iso,end_iso,label,note\n2026-09-23T00:00:00,2026-09-23T00:01:00+07:00,spike,",
        "start_iso,end_iso,label,note\n2026-09-23T00:00:00+07:00,2026-09-23T00:00:00+07:00,spike,",
        "start_iso,end_iso,label,note\n2026-09-23T00:00:00+07:00,2026-09-23T00:01:00+07:00,,",
    ],
)
def test_invalid_events(output_dir, content):
    with pytest.raises(ValueError):
        load_events(write(output_dir / "events.csv", content))


def test_empty_events(output_dir):
    assert load_events(
        write(output_dir / "events.csv", "start_iso,end_iso,label,note")
    ).empty
