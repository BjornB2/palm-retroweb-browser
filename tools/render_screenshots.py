#!/usr/bin/env python3
"""Build deterministic 160x160 Palm OS browser screenshots as SVG.

The source image is the GIF returned by RetroWEB's image cruncher.  Keeping the
layout as SVG makes the screenshot reproducible without a Palm emulator; the
Makefile converts it to PNG for GitHub.
"""

import base64
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_GIF = ROOT / "assets" / "screenshots" / "source" / "ixfilm-retroweb.gif"
OUTPUT_DIR = ROOT / "assets" / "screenshots"


def screen(body: str, thumb_y: int, thumb_height: int) -> str:
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="160" height="160" viewBox="0 0 160 160">
  <defs>
    <clipPath id="page"><rect x="2" y="16" width="146" height="140"/></clipPath>
  </defs>
  <rect width="160" height="160" fill="#fff"/>
  <rect width="160" height="14" fill="#292929"/>
  <text x="3" y="10.5" fill="#fff" font-family="Geneva, sans-serif" font-size="9">Page loaded</text>
  <g clip-path="url(#page)" font-family="Geneva, sans-serif" fill="#111">{body}</g>
  <rect x="151" y="16" width="7" height="140" fill="#eee" stroke="#777" stroke-width="0.5"/>
  <path d="M154.5 17 L151.5 22 L157.5 22 Z" fill="#555"/>
  <path d="M154.5 155 L151.5 150 L157.5 150 Z" fill="#555"/>
  <rect x="152" y="{thumb_y}" width="5" height="{thumb_height}" rx="1" fill="#777"/>
</svg>'''


def main() -> None:
    if not SOURCE_GIF.exists():
        raise SystemExit(
            "Missing the RetroWEB-converted Bjorns Blog hero image."
        )

    gif = base64.b64encode(SOURCE_GIF.read_bytes()).decode("ascii")
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    image = f"data:image/gif;base64,{gif}"
    (OUTPUT_DIR / "bjornsblog-palm-160x160.svg").write_text(
        screen(f'''
    <text x="4" y="26" font-size="12" font-weight="bold">Björns Blog</text>
    <image x="4" y="31" width="200" height="150" href="{image}"/>
    <rect x="4" y="31" width="142" height="89" fill="none" stroke="#777" stroke-width="0.5"/>
''', 24, 35),
        encoding="utf-8",
    )
    (OUTPUT_DIR / "bjornsblog-article-160x160.svg").write_text(
        screen(f'''
    <image x="4" y="-82" width="200" height="150" href="{image}"/>
    <text x="4" y="82" font-size="9" fill="#173f9f">Photography</text>
    <text x="4" y="96" font-size="11" font-weight="bold" fill="#173f9f">Film simulations on a</text>
    <text x="4" y="109" font-size="11" font-weight="bold" fill="#173f9f">tiny Canon IXUS with</text>
    <text x="4" y="122" font-size="11" font-weight="bold" fill="#173f9f">IXFilm OS</text>
    <line x1="4" y1="124" x2="135" y2="124" stroke="#173f9f"/>
    <text x="4" y="137" font-size="8">IXFilm OS adds in-camera</text>
    <text x="4" y="148" font-size="8">film simulations...</text>
''', 64, 35),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
