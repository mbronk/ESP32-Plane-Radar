 #!/usr/bin/env python3
"""Regenerate data/ui_font.vlw, adding Polish (Latin Extended-A) glyphs.

The existing embedded smooth font (Noto Sans Bold, size 15) only contains
ASCII 0x21-0x7E plus the degree sign (0xB0). This script rasterizes the
missing Polish diacritic glyphs from a locally installed bold sans-serif
TrueType font (Arial Bold, closest available match) and merges them into
the existing VLW glyph table, preserving all original glyph bitmaps
byte-for-byte.

VLW format (reverse-engineered from LovyanGFX's VLWfont::loadFont /
drawChar in lgfx_fonts.cpp):

Header (24 bytes, 6 x big-endian int32):
    gCount, version, size(point size), discard, ascent, descent

Glyph metrics table (gCount records, 28 bytes each, 7 x big-endian int32),
sorted ascending by unicode codepoint (required — runtime does a binary
search over this array):
    unicode, height, width, xAdvance, dY (top offset above baseline),
    dX (left bearing), reserved(0)

Bitmap data: for each glyph in table order, width*height bytes of 8-bit
alpha (0 = background/transparent, 255 = fully opaque foreground),
concatenated with no padding.
"""
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO_ROOT = Path(__file__).resolve().parent.parent
VLW_PATH = REPO_ROOT / "data" / "ui_font.vlw"
FONT_PATH = Path(r"C:\Windows\Fonts\arialbd.ttf")
FONT_SIZE = 15  # matches original font's declared point size / cap-height

# Polish Latin Extended-A letters missing from the embedded font.
POLISH_CHARS = list("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ")


def parse_vlw(data: bytes):
    gCount, version, size, discard, ascent, descent = struct.unpack(">iiiiii", data[0:24])
    header = {
        "gCount": gCount, "version": version, "size": size,
        "discard": discard, "ascent": ascent, "descent": descent,
    }
    records = []
    off = 24
    for _ in range(gCount):
        unicode_, height, width, xAdvance, dY, dX, reserved = struct.unpack(
            ">7i", data[off:off + 28])
        records.append({
            "unicode": unicode_, "height": height, "width": width,
            "xAdvance": xAdvance, "dY": dY, "dX": dX, "reserved": reserved,
        })
        off += 28
    bitmaps = []
    for rec in records:
        n = rec["width"] * rec["height"]
        bitmaps.append(data[off:off + n])
        off += n
    # A short name/postscript-name footer (Processing/VLW encoder metadata)
    # follows the bitmap data. It is never read by the LovyanGFX runtime
    # loader, but we preserve it unchanged for a well-formed container.
    footer = data[off:]
    return header, records, bitmaps, footer


def rasterize_glyph(font: ImageFont.FreeTypeFont, ch: str):
    # Use a generously sized scratch canvas anchored at the baseline so the
    # ink bbox can be measured precisely (top is negative = above baseline).
    scratch = Image.new("L", (64, 64), 0)
    draw = ImageDraw.Draw(scratch)
    origin = (16, 32)
    bbox = draw.textbbox(origin, ch, font=font, anchor="ls")
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    if width <= 0 or height <= 0:
        # Glyph has no ink (shouldn't happen for the Polish set) — 1x1 blank.
        return {"width": 0, "height": 0, "dX": 0, "dY": 0}, b""

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
    data = VLW_PATH.read_bytes()
    header, records, bitmaps, footer = parse_vlw(data)
    print(f"Existing font: gCount={header['gCount']} size={header['size']} "
          f"ascent={header['ascent']} descent={header['descent']} "
          f"footer={footer!r}")

    existing_codepoints = {r["unicode"] for r in records}
    font = ImageFont.truetype(str(FONT_PATH), FONT_SIZE)

    new_records = []
    new_bitmaps = []
    for ch in POLISH_CHARS:
        cp = ord(ch)
        if cp in existing_codepoints:
            print(f"skip U+{cp:04X} ({ch}) — already present")
            continue
        metrics, pixels = rasterize_glyph(font, ch)
        rec = {
            "unicode": cp,
            "height": metrics["height"],
            "width": metrics["width"],
            "xAdvance": metrics["xAdvance"],
            "dY": metrics["dY"],
            "dX": metrics["dX"],
            "reserved": 0,
        }
        new_records.append(rec)
        new_bitmaps.append(pixels)
        print(f"add  U+{cp:04X} ({ch}) w={rec['width']} h={rec['height']} "
              f"xAdv={rec['xAdvance']} dY={rec['dY']} dX={rec['dX']}")

    all_records = records + new_records
    all_bitmaps = bitmaps + new_bitmaps
    order = sorted(range(len(all_records)), key=lambda i: all_records[i]["unicode"])
    all_records = [all_records[i] for i in order]
    all_bitmaps = [all_bitmaps[i] for i in order]

    gCount = len(all_records)
    out = bytearray()
    out += struct.pack(">iiiiii", gCount, header["version"], header["size"],
                        header["discard"], header["ascent"], header["descent"])
    for rec in all_records:
        out += struct.pack(">7i", rec["unicode"], rec["height"], rec["width"],
                            rec["xAdvance"], rec["dY"], rec["dX"], rec["reserved"])
    for bm in all_bitmaps:
        out += bm
    out += footer

    backup = VLW_PATH.with_suffix(".vlw.bak")
    if not backup.exists():
        backup.write_bytes(data)
        print(f"Backed up original font to {backup}")

    VLW_PATH.write_bytes(bytes(out))
    print(f"Wrote {VLW_PATH} — {gCount} glyphs, {len(out)} bytes "
          f"(was {len(data)} bytes)")


if __name__ == "__main__":
    main()
