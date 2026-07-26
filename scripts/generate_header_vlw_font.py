#!/usr/bin/env python3
"""Generate data/ui_header_font.vlw — a dedicated, larger smooth font for
the weather page's condition-label header (e.g. "Czesciowo zachm.").

Unlike scripts/regenerate_vlw_font.py (which *patches* Polish glyphs into
the existing small body font), this script rasterizes a complete glyph set
from scratch at a bigger point size. Reusing the body font's native 15pt
bitmaps and stretching them via setTextSize() produces blurry/blocky text
once the scale factor is non-integer (e.g. 1.3x), because VLWfont::drawChar
scales the small source alpha bitmap with simple fixed-point pixel
replication — there's no extra source detail to scale up cleanly. A second
font rasterized directly at the target size avoids that entirely (drawn at
scale 1.0, so no runtime stretching happens).

Produces the same VLW container format as regenerate_vlw_font.py (see that
script's docstring for the on-disk layout).
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO_ROOT = Path(__file__).resolve().parent.parent
VLW_PATH = REPO_ROOT / "data" / "ui_header_font.vlw"
FONT_PATH = Path(r"C:\Windows\Fonts\arialbd.ttf")
FONT_SIZE = 24  # rasterized point size — bigger than the 15pt body font so
                # it can be drawn at setTextSize(1.0) without upscaling blur.

# Full ASCII glyph range used elsewhere in this codebase, plus degree sign
# and the Polish Latin Extended-A letters needed for condition labels.
# 0x20 (space) is intentionally omitted — VLWfont handles it as a special
# case (advances by a computed spaceWidth) rather than a real glyph.
ASCII_CHARS = [chr(c) for c in range(0x21, 0x7F)]
EXTRA_CHARS = ["\u00b0"]  # degree sign
POLISH_CHARS = list("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ")
ALL_CHARS = ASCII_CHARS + EXTRA_CHARS + POLISH_CHARS


def rasterize_glyph(font: ImageFont.FreeTypeFont, ch: str):
    # Generously sized scratch canvas anchored at the baseline so the ink
    # bbox can be measured precisely (top is negative = above baseline).
    scratch = Image.new("L", (96, 96), 0)
    draw = ImageDraw.Draw(scratch)
    origin = (24, 48)
    bbox = draw.textbbox(origin, ch, font=font, anchor="ls")
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    if width <= 0 or height <= 0:
        return {"width": 0, "height": 0, "dX": 0, "dY": 0, "xAdvance": round(font.getlength(ch))}, b""

    draw.text(origin, ch, font=font, fill=255, anchor="ls")
    crop = scratch.crop((left, top, right, bottom))
    pixels = bytes(crop.getdata())

    dY = origin[1] - top          # distance from baseline up to top of ink
    dX = left - origin[0]         # left bearing relative to pen position
    x_advance = round(font.getlength(ch))
    return {
        "width": width, "height": height, "dX": dX, "dY": dY,
        "xAdvance": x_advance,
    }, pixels


def main():
    font = ImageFont.truetype(str(FONT_PATH), FONT_SIZE)

    records = []
    bitmaps = []
    for ch in ALL_CHARS:
        cp = ord(ch)
        metrics, pixels = rasterize_glyph(font, ch)
        records.append({
            "unicode": cp,
            "height": metrics["height"],
            "width": metrics["width"],
            "xAdvance": metrics["xAdvance"],
            "dY": metrics["dY"],
            "dX": metrics["dX"],
            "reserved": 0,
        })
        bitmaps.append(pixels)

    order = sorted(range(len(records)), key=lambda i: records[i]["unicode"])
    records = [records[i] for i in order]
    bitmaps = [bitmaps[i] for i in order]

    gCount = len(records)
    ascent = max(r["dY"] for r in records)
    descent = max(r["height"] - r["dY"] for r in records)

    out = bytearray()
    out += struct.pack(">iiiiii", gCount, 11, FONT_SIZE, 0, ascent, descent)
    for rec in records:
        out += struct.pack(">7i", rec["unicode"], rec["height"], rec["width"],
                            rec["xAdvance"], rec["dY"], rec["dX"], rec["reserved"])
    for bm in bitmaps:
        out += bm

    VLW_PATH.write_bytes(bytes(out))
    print(f"Wrote {VLW_PATH} — {gCount} glyphs, {len(out)} bytes, "
          f"ascent={ascent} descent={descent}")


if __name__ == "__main__":
    main()
