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
SECTION = re.compile(r'<section class="slide[^"]*" data-dur="(\d+)".*?</section>', re.S)
NOTES = '<aside class="notes">'
TAG = re.compile(r"<[^>]+>")


def say_html(speaker, text):
    """Script text -> notes HTML; [stage directions] are set apart."""
    body = html.escape(text.strip()).replace("\n", "<br>")
    body = re.sub(r"\[([^\]]+)\]", r'<i class="dir">[\1]</i>', body)
    return f'<div class="say"><h5>บทพูด · ผู้พูดแนะนำ: {html.escape(speaker)}</h5><p>{body}</p></div>'


def add_script(text):
    sys.path.insert(0, str(HERE / "src"))
    from script import SCRIPT
    n = 0

    def one(m):
        nonlocal n
        n += 1
        sec = m.group(0)
        if n in SCRIPT and NOTES in sec:
            sec = sec.replace(NOTES, NOTES + say_html(*SCRIPT[n]), 1)
        return sec

    return SECTION.sub(one, text)


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
    # same deck in the dark cool theme, for comparison (CSS under body.theme-dark in the source)
    dark = out.replace("<body>", '<body class="theme-dark">', 1).replace("<title>AirGuard-S3</title>", "<title>AirGuard-S3 · dark</title>", 1)
    OUT_DARK.write_text(dark, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_DARK.relative_to(ROOT)}")
    write_script_page(out)
    print(f"wrote {SCRIPT_OUT.relative_to(ROOT)}")
    print(f"embedded {len(found)}:", *found, sep="\n  ")
    print(f"missing {len(missing)}:", *missing, sep="\n  ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
