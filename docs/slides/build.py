"""Build docs/slides/AirGuard-S3.html from src/AirGuard-S3.src.html.

Every <figure class="ph" data-img="path/without-or-with-ext" data-label="..."></figure>
in the source becomes an embedded image (data URI) when the file exists under
the repo root, otherwise a visible "[ใส่รูป: ...]" frame. Drop new photos or
schematic sheets into docs/slides/img/ with the names used in the source and
run this again:

    python docs/slides/build.py

Prints which figures were embedded and which are still missing.
"""
import base64
import html
import json
import mimetypes
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SRC = HERE / "src" / "AirGuard-S3.src.html"
OUT = HERE / "AirGuard-S3.html"
OUT_DARK = HERE / "AirGuard-S3-dark.html"
OUT_CARTOON = HERE / "AirGuard-S3-cartoon.html"
EXTS = ("", ".svg", ".png", ".jpg", ".jpeg", ".webp")
FIG = re.compile(r'<figure class="ph([^"]*)" data-img="([^"]+)" data-label="([^"]+)"></figure>')
GEO = re.compile(r'<script type="application/json" id="encl-geo" data-from="([^"]+)" data-keys="([^"]+)"></script>')


def find(rel):
    for ext in EXTS:
        p = ROOT / (rel + ext)
        if p.is_file():
            return p
    return None


SCRIPT_OUT = HERE / "AirGuard-S3-script.html"
SECTION = re.compile(r'<section class="slide[^"]*" data-dur="(\d+)"(?: data-sid="([^"]+)")?.*?</section>', re.S)
NOTES = '<aside class="notes">'
TAG = re.compile(r"<[^>]+>")


def say_html(speaker, text):
    """Script text -> notes HTML; [stage directions] are set apart."""
    body = html.escape(text.strip()).replace("\n", "<br>")
    body = re.sub(r"\[([^\]]+)\]", r'<i class="dir">[\1]</i>', body)
    return f'<div class="say"><h5>บทพูด · ผู้พูดแนะนำ: {html.escape(speaker)}</h5><p>{body}</p></div>'


# slides that also show their script on the slide itself (firmware part, slides 9–14)
ON_SLIDE = {"fw_tasks", "fw_isr", "fw_tick", "sens", "fw_mem", "co"}
SRC_DIV = '<div class="src">'


def say_on_html(text):
    """Script -> one visible paragraph: hand-off lines and [stage directions] left in the notes only."""
    lines = [ln for ln in text.strip().splitlines() if not ln.startswith("[ส่งต่อ]")]
    body = re.sub(r"\s*\[[^\]]+\]\s*", " ", " ".join(lines)).strip()
    return f'<div class="say-on">{html.escape(body)}</div>\n  '


def add_script(text):
    sys.path.insert(0, str(HERE / "src"))
    from script import SCRIPT
    seen = set()

    def one(m):
        sec, sid = m.group(0), m.group(2)
        seen.add(sid)
        if sid in SCRIPT and NOTES in sec:
            sec = sec.replace(NOTES, NOTES + say_html(*SCRIPT[sid]), 1)
        if sid in ON_SLIDE and SRC_DIV in sec:
            sec = sec.replace(SRC_DIV, say_on_html(SCRIPT[sid][1]) + SRC_DIV, 1)
            sec = sec.replace('<section class="slide', '<section class="slide has-say', 1)
        return sec

    out = SECTION.sub(one, text)
    unused = sorted(set(SCRIPT) - seen)
    if unused:
        print("script entries with no slide:", ", ".join(unused))
    return out


SCRIPT_MD = HERE / "AirGuard-S3-script.md"


def mmss(s):
    return f"{s // 60}:{s % 60:02d}"


def write_script_md(deck):
    """The speaking script as one Markdown document, from the deck + src/script.py."""
    from script import SCRIPT, MUST, DEMO_RUN, DEMO_BEFORE, DEMO_BACKUP, QA
    rows, parts, t, talk = [], [], 0, 0
    for i, m in enumerate(SECTION.finditer(deck), 1):
        sec, dur, sid = m.group(0), int(m.group(1)), m.group(2)
        if sid == "apx" or not dur:
            break
        title = TAG.sub("", re.search(r"<h[12][^>]*>(.*?)</h[12]>", sec, re.S).group(1)).strip()
        who, text = SCRIPT[sid]
        rows.append(f"| {i} | {title} | {mmss(t)} | {mmss(dur)} | {who} |")
        part = [f"### {i} · {title} · {mmss(dur)} · {who}", "", f"**ต้องพูดถึง:** {MUST[sid]}", "",
                "**บทพูด:** " + " ".join(text.split())]
        if sid == "demo":
            part += ["", "**ก่อนเริ่ม:**", ""] + [f"- [ ] {x}" for x in DEMO_BEFORE]
            part += ["", "| เวลา | ใคร | ทำอะไร · พูดอะไร |", "| --- | --- | --- |"]
            part += [f"| {a} | {b} | {c} |" for a, b, c in DEMO_RUN]
            part += ["", f"**แผนสำรอง:** {DEMO_BACKUP}"]
        parts.append("\n".join(part))
        t += dur
        talk += 0 if sid == "demo" else dur
    md = [
        "# AirGuard-S3 · บทพูดนำเสนอ 5 พ.ย. 2569", "",
        f"พูด {mmss(talk)} นาที + demo สด 10:00 นาที รวม {mmss(t)} นาที · สามคน: A = ฮาร์ดแวร์ · B = เฟิร์มแวร์ · "
        "C = ข้อมูลและโมเดล · ข้อความใน [ ] คือคำแนะนำ ไม่ต้องอ่านออกเสียง · หน้ารายละเอียดอยู่ในภาคผนวกท้ายสไลด์ ใช้ตอบคำถาม", "",
        "สร้างจาก `docs/slides/src/script.py` ด้วย `python docs/slides/build.py` (บทเดียวกับโน้ตในสไลด์ กด N) — แก้ที่ script.py แล้ว build ใหม่", "",
        "## ตารางเวลา", "", "| หน้า | หัวข้อ | เริ่ม | ยาว | ผู้พูด |", "| --- | --- | --- | --- | --- |", *rows, "",
        "## ต้องพูดอะไรในแต่ละหน้า", "", "\n\n".join(parts), "",
        "## คำถามที่น่าจะโดน", "", "| คำถาม | ตอบสั้นๆ | หน้า |", "| --- | --- | --- |",
        *[f"| {q} | {a} | {p} |" for q, a, p in QA], "",
    ]
    SCRIPT_MD.write_text("\n".join(md), encoding="utf-8", newline="\n")


def write_script_page(deck):
    rows, t = [], 0
    for i, m in enumerate(SECTION.finditer(deck), 1):
        sec, dur = m.group(0), int(m.group(1))
        title = re.search(r"<h[12][^>]*>(.*?)</h[12]>", sec, re.S)
        cat = re.search(r'<span class="cat">(.*?)</span>', sec)
        notes = re.search(r'<aside class="notes">(.*?)</aside>', sec, re.S)
        say = re.search(r'<div class="say">.*?<p>(.*?)</p>', sec, re.S)
        chars = len(re.sub(r"\s|\[.*?\]", "", TAG.sub("", re.sub(r'<i class="dir">.*?</i>', "", say.group(1))))) if say else 0
        rate = f"{chars / dur:.1f} ตัวอักษร/วินาที" if dur and chars else ""
        rows.append(
            f'<section><h2>{i}. {TAG.sub("", title.group(1)) if title else ""}</h2>'
            f'<div class="meta">{TAG.sub("", cat.group(1)) if cat else ""} · เริ่ม {t // 60}:{t % 60:02d} · '
            f'ยาว {dur // 60}:{dur % 60:02d} {("· " + rate) if rate else ""}</div>'
            f'{notes.group(1) if notes else ""}</section>')
        t += dur
    SCRIPT_OUT.write_text(
        '<!doctype html><html lang="th"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1"><title>AirGuard-S3 บทพูด</title><style>'
        'body{margin:0;background:#fff;color:#16213a;font:17px/1.65 "Leelawadee UI","Segoe UI",Tahoma,sans-serif}'
        'main{max-width:860px;margin:0 auto;padding:24px 16px 60px}'
        'h1{font-size:28px;margin:0 0 4px}h2{font-size:21px;margin:0 0 2px}'
        'section{border-top:1px solid #d8dde7;padding:16px 0;break-inside:avoid}'
        '.meta{font-size:14px;color:#5a6478;margin-bottom:8px}'
        '.say{background:#f3f5f9;border-left:5px solid #1d4ed8;border-radius:8px;padding:10px 14px;margin-bottom:8px}'
        '.say h5{margin:0 0 4px;font-size:14px;color:#1d4ed8}.say p{margin:0}.dir{color:#b45309;font-style:normal}'
        'ul{margin:4px 0;padding-left:1.2em}li{margin:2px 0;font-size:15px}p{margin:4px 0}'
        '@media print{main{max-width:none;padding:0}}</style></head><body><main>'
        f'<h1>AirGuard-S3 · บทพูด</h1><div class="meta">รวม {t // 60}:{t % 60:02d} นาที · สร้างจาก docs/slides/src '
        '(python docs/slides/build.py) · ข้อความใน [ ] คือคำแนะนำ ไม่ต้องอ่านออกเสียง</div>'
        + "".join(rows) + "</main></body></html>",
        encoding="utf-8", newline="\n")


def main():
    text = SRC.read_text(encoding="utf-8")
    found, missing = [], []

    def repl(m):
        extra, rel, label = m.group(1), m.group(2), html.unescape(m.group(3))
        p = find(rel)
        if p is None:
            missing.append(rel)
            return (f'<figure class="ph missing{extra}"><div>[ใส่รูป: {html.escape(label)}]'
                    f'<small>{html.escape(rel)}.*</small></div></figure>')
        mime = mimetypes.guess_type(p.name)[0] or "application/octet-stream"
        if p.suffix == ".svg":
            mime = "image/svg+xml"
        data = base64.b64encode(p.read_bytes()).decode("ascii")
        found.append(str(p.relative_to(ROOT)))
        return (f'<figure class="img{extra}"><img alt="{html.escape(label)}" '
                f'src="data:{mime};base64,{data}"></figure>')

    def geo(m):
        # slide 25: copy the enclosure meshes (base64 float32 triangle soup) out of the 3D viewer page
        rel, keys = m.group(1), m.group(2).split(",")
        p = ROOT / rel
        if not p.is_file():
            missing.append(rel)
            return m.group(0)
        src = p.read_text(encoding="utf-8")
        blob = re.search(r'<script[^>]*id="geo"[^>]*>(.*?)</script>', src, re.S)
        allgeo = json.loads(blob.group(1)) if blob else {}
        sel = {k: allgeo[k] for k in keys if k in allgeo}
        if len(sel) != len(keys):
            missing.append(rel + " (geo " + ",".join(k for k in keys if k not in sel) + ")")
        found.append(f"{rel} (geo {', '.join(sel)})")
        return f'<script type="application/json" id="encl-geo">{json.dumps(sel, separators=(",", ":"))}</script>'

    text = GEO.sub(geo, text)
    out = add_script(FIG.sub(repl, text))
    OUT.write_text(out, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size:,} bytes)")
    # the same deck in other themes, for comparison (CSS under body.theme-<name> in the source)
    for path, theme in ((OUT_DARK, "dark"), (OUT_CARTOON, "cartoon")):
        alt = out.replace("<body>", f'<body class="theme-{theme}">', 1).replace(
            "<title>AirGuard-S3</title>", f"<title>AirGuard-S3 · {theme}</title>", 1)
        path.write_text(alt, encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(ROOT)}")
    write_script_page(out)
    write_script_md(out)
    print(f"wrote {SCRIPT_MD.relative_to(ROOT)}")
    print(f"wrote {SCRIPT_OUT.relative_to(ROOT)}")
    print(f"embedded {len(found)}:", *found, sep="\n  ")
    print(f"missing {len(missing)}:", *missing, sep="\n  ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
