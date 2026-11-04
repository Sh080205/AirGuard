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
import mimetypes
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SRC = HERE / "src" / "AirGuard-S3.src.html"
OUT = HERE / "AirGuard-S3.html"
EXTS = ("", ".svg", ".png", ".jpg", ".jpeg", ".webp")
FIG = re.compile(r'<figure class="ph([^"]*)" data-img="([^"]+)" data-label="([^"]+)"></figure>')


def find(rel):
    for ext in EXTS:
        p = ROOT / (rel + ext)
        if p.is_file():
            return p
    return None


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

    out = FIG.sub(repl, text)
    OUT.write_text(out, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size:,} bytes)")
    print(f"embedded {len(found)}:", *found, sep="\n  ")
    print(f"missing {len(missing)}:", *missing, sep="\n  ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
