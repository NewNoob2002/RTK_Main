#!/usr/bin/env python3
"""Inline main/web/ui/ assets into a single HTML file and gzip it (<= 24 KB)."""

import argparse
import base64
import gzip
import pathlib
import re
import sys

MAX_GZIP_BYTES = 24 * 1024


def pack_ui(ui_dir: pathlib.Path) -> bytes:
    """Read index.html, inline app.css, app.js and logo.png, and return UTF-8 HTML bytes."""
    html = (ui_dir / "index.html").read_text(encoding="utf-8")
    css = (ui_dir / "app.css").read_text(encoding="utf-8").strip()
    js = (ui_dir / "app.js").read_text(encoding="utf-8").strip()
    logo_b64 = base64.b64encode((ui_dir / "logo.png").read_bytes()).decode("ascii")

    html = re.sub(
        r'<link\s+rel="stylesheet"\s+href="app\.css"\s*/?>',
        lambda _m: f"<style>\n{css}\n</style>",
        html,
        count=1,
    )
    html = re.sub(
        r'<script\s+src="app\.js"\s*></script>',
        lambda _m: f"<script>\n{js}\n</script>",
        html,
        count=1,
    )
    html = html.replace('src="logo.png"', f'src="data:image/png;base64,{logo_b64}"', 1)
    return html.encode("utf-8")


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Pack and gzip web UI.")
    parser.add_argument(
        "--ui-dir",
        type=pathlib.Path,
        default=repo_root / "main" / "web" / "ui",
        help="Path to web UI source directory",
    )
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=repo_root / "builds" / "w4-pack" / "index.html.gz",
        help="Output .html.gz path",
    )
    parser.add_argument(
        "--out-html",
        type=pathlib.Path,
        default=None,
        help="Optional output path for unpacked inlined HTML",
    )
    args = parser.parse_args()

    raw = pack_ui(args.ui_dir)
    gz = gzip.compress(raw, compresslevel=9, mtime=0)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(gz)
    if args.out_html is not None:
        args.out_html.parent.mkdir(parents=True, exist_ok=True)
        args.out_html.write_bytes(raw)

    print(
        f"raw={len(raw)} B, gzip={len(gz)} B / {MAX_GZIP_BYTES} B "
        f"({len(gz) * 100.0 / MAX_GZIP_BYTES:.1f}%)"
    )
    if len(gz) > MAX_GZIP_BYTES:
        print(f"ERROR: gzip size {len(gz)} exceeds {MAX_GZIP_BYTES} B", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
