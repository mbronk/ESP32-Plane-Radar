#!/usr/bin/env python3
"""Render a PNG preview of every weather condition icon and the sunrise/
sunset status icons, so they can be reviewed on a PC without flashing the
device.

All icons are simple hand-drawn vector primitives (circles, wide lines,
triangles, a rect) drawn at runtime in src/ui/weather_display.cpp — there
are no external icon assets/fonts involved. This script is a 1:1 port of
those same drawing calls (same coordinates/radii) using Pillow, purely for
fast iteration/preview. If you change the C++ drawing code, update the
matching function here to keep the preview accurate.

Usage:
    python scripts/render_icons_preview.py
    (writes scripts/output/icon_preview.png and opens it)
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "scripts" / "output"
OUT_PATH = OUT_DIR / "icon_preview.png"

CELL = 90        # px per icon cell
COLS = 5
BG = (18, 20, 26)
LABEL_COLOR = (200, 205, 215)

SUN = (255, 200, 0)
CLOUD = (205, 210, 220)
CLOUD_DARK = (120, 132, 150)
RAIN = (70, 150, 255)
SNOW = (240, 245, 255)
BOLT = (255, 215, 0)
FOG = (180, 190, 200)
ICY = (180, 220, 255)
HAIL_COLOR = (225, 230, 240)


def wide_line(draw, x1, y1, x2, y2, w, color):
    draw.line([(x1, y1), (x2, y2)], fill=color, width=max(1, round(w)))


def draw_sun(draw, cx, cy, r, color):
    for a in range(0, 360, 45):
        rad = math.radians(a)
        x1 = cx + round(math.cos(rad) * (r + 4))
        y1 = cy + round(math.sin(rad) * (r + 4))
        x2 = cx + round(math.cos(rad) * (r + 11))
        y2 = cy + round(math.sin(rad) * (r + 11))
        wide_line(draw, x1, y1, x2, y2, 1.5, color)
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)


def draw_cloud(draw, cx, cy, color):
    # Flat-bottomed body drawn as a single rounded rect (gently rounded
    # corners, not full-height circles -- those made the base circles'
    # shoulders look like two bumps/wheels flanking the flat bottom), plus
    # puffy uneven circles on top for the cloud silhouette.
    body_w = 52
    body_h = 22
    body_r = 8
    draw.rounded_rectangle(
        [cx - body_w // 2, cy - 4, cx - body_w // 2 + body_w, cy - 4 + body_h],
        radius=body_r, fill=color)
    draw.ellipse([cx - 10 - 12, cy - 5 - 12, cx - 10 + 12, cy - 5 + 12], fill=color)
    draw.ellipse([cx + 10 - 13, cy - 6 - 13, cx + 10 + 13, cy - 6 + 13], fill=color)
    draw.ellipse([cx + 1 - 12, cy - 12 - 12, cx + 1 + 12, cy - 12 + 12], fill=color)


def intensity_slots(cx, spacing, intensity):
    if intensity == "Light":
        return [cx]
    if intensity == "Heavy":
        return [cx - spacing, cx, cx + spacing]
    return [cx - spacing, cx + spacing]


def draw_raindrops(draw, cx, cy, color, intensity):
    xs = intensity_slots(cx, 14, intensity)
    thick = 2.0 if intensity == "Heavy" else 1.5
    for x in xs:
        wide_line(draw, x, cy, x - 4, cy + 12, thick, color)


def draw_drizzle_dots(draw, cx, cy, color, intensity):
    xs = intensity_slots(cx, 14, intensity)
    for x in xs:
        r = 2
        draw.ellipse([x - r, cy - r, x + r, cy + r], fill=color)
        draw.ellipse([x - 1, cy + 7, x + 1, cy + 9], fill=color)


def draw_snowflakes(draw, cx, cy, color, intensity):
    xs = intensity_slots(cx, 14, intensity)
    for x in xs:
        y = cy + 6
        wide_line(draw, x - 4, y, x + 4, y, 1.2, color)
        wide_line(draw, x - 3, y - 3, x + 3, y + 3, 1.2, color)
        wide_line(draw, x - 3, y + 3, x + 3, y - 3, 1.2, color)


def draw_ice_pellets(draw, cx, cy, color, intensity):
    xs = intensity_slots(cx, 14, intensity)
    for x in xs:
        wide_line(draw, x, cy, x, cy + 10, 1.5, color)
        r = 2
        draw.ellipse([x - r, cy + 12 - r, x + r, cy + 12 + r], fill=color)


def draw_hail_pellets(draw, cx, cy, color, intensity):
    xs = intensity_slots(cx, 12, intensity)
    for x in xs:
        r = 3
        draw.ellipse([x - r, cy - r, x + r, cy + r], fill=color)



def draw_bolt(draw, cx, cy, color):
    draw.polygon([(cx - 5, cy), (cx + 6, cy), (cx - 2, cy + 14)], fill=color)
    draw.polygon([(cx + 3, cy + 8), (cx + 11, cy + 8), (cx - 3, cy + 26)], fill=color)


def draw_sun_event(draw, x, y, sunrise, color):
    w = 14
    r = 4
    cx = x + w // 2
    cy = y - 2
    horizon = y
    draw.line([(x, horizon), (x + w, horizon)], fill=color, width=1)
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)
    if sunrise:
        for dx in (-8, 0, 8):
            wide_line(draw, cx, cy, cx + dx, cy - 8, 1.2, color)
    else:
        for dx in (-6, 0, 6):
            wide_line(draw, cx, cy, cx + dx, cy + 8, 1.2, color)


def draw_condition_icon(draw, cond, intensity, cx, cy):
    if cond == "Clear":
        draw_sun(draw, cx, cy, 20, SUN)
    elif cond == "PartlyCloudy":
        draw_sun(draw, cx + 12, cy - 12, 12, SUN)
        draw_cloud(draw, cx - 2, cy + 6, CLOUD)
    elif cond == "Cloudy":
        draw_cloud(draw, cx, cy + 2, CLOUD)
    elif cond == "Fog":
        draw_cloud(draw, cx, cy - 6, FOG)
        for i in range(3):
            y = cy + 16 + i * 7
            wide_line(draw, cx - 22, y, cx + 22, y, 1.5, FOG)
    elif cond == "Drizzle":
        draw_cloud(draw, cx, cy - 6, CLOUD)
        draw_drizzle_dots(draw, cx, cy + 14, RAIN, intensity)
    elif cond == "Rain":
        draw_cloud(draw, cx, cy - 6, CLOUD_DARK)
        draw_raindrops(draw, cx, cy + 14, RAIN, intensity)
    elif cond == "FreezingRain":
        draw_cloud(draw, cx, cy - 6, CLOUD_DARK)
        draw_ice_pellets(draw, cx, cy + 12, ICY, intensity)
    elif cond == "Snow":
        draw_cloud(draw, cx, cy - 6, CLOUD)
        draw_snowflakes(draw, cx, cy + 12, SNOW, intensity)
    elif cond == "Storm":
        draw_cloud(draw, cx, cy - 6, CLOUD_DARK)
        draw_bolt(draw, cx, cy + 12, BOLT)
    elif cond == "Hail":
        draw_cloud(draw, cx, cy - 6, CLOUD_DARK)
        draw_bolt(draw, cx - 8, cy + 10, BOLT)
        draw_hail_pellets(draw, cx + 10, cy + 20, HAIL_COLOR, intensity)
    else:  # Unknown
        draw.ellipse([cx - 18, cy - 18, cx + 18, cy + 18], outline=FOG, width=2)


CONDITIONS = ["Clear", "PartlyCloudy", "Cloudy", "Fog", "Drizzle", "Rain",
              "FreezingRain", "Snow", "Storm", "Hail", "Unknown"]

# Conditions whose icon actually varies with intensity (matches drawIcon()'s
# use of intensitySlots() in weather_display.cpp).
INTENSITY_CONDITIONS = ["Drizzle", "Rain", "FreezingRain", "Snow", "Hail"]
INTENSITIES = ["Light", "Moderate", "Heavy"]


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    top_rows = math.ceil(len(CONDITIONS) / COLS)
    variant_rows = len(INTENSITY_CONDITIONS)
    variant_cols = len(INTENSITIES) + 1  # +1 for a row label column
    sun_row_h = CELL
    header_h = 26

    total_h = (header_h + top_rows * CELL + header_h +
               variant_rows * CELL + header_h + sun_row_h)
    grid_w = max(COLS * CELL, variant_cols * CELL)
    img = Image.new("RGB", (grid_w, total_h), BG)
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(r"C:\Windows\Fonts\arial.ttf", 13)
        header_font = ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf", 15)
    except OSError:
        font = ImageFont.load_default()
        header_font = font

    y_cursor = 0
    draw.text((10, y_cursor + header_h // 2), "All conditions (Moderate intensity)",
               fill=LABEL_COLOR, font=header_font, anchor="lm")
    y_cursor += header_h

    for idx, cond in enumerate(CONDITIONS):
        col = idx % COLS
        row = idx // COLS
        cx = col * CELL + CELL // 2
        cy = y_cursor + row * CELL + CELL // 2 - 6
        draw_condition_icon(draw, cond, "Moderate", cx, cy)
        draw.text((col * CELL + CELL // 2, y_cursor + row * CELL + CELL - 14),
                   cond, fill=LABEL_COLOR, font=font, anchor="mm")
    y_cursor += top_rows * CELL

    draw.text((10, y_cursor + header_h // 2),
               "Intensity variants (Light / Moderate / Heavy)",
               fill=LABEL_COLOR, font=header_font, anchor="lm")
    y_cursor += header_h

    for row, cond in enumerate(INTENSITY_CONDITIONS):
        row_cy = y_cursor + row * CELL + CELL // 2
        draw.text((CELL // 2, row_cy), cond, fill=LABEL_COLOR, font=font,
                   anchor="mm")
        for col, intensity in enumerate(INTENSITIES):
            cx = (col + 1) * CELL + CELL // 2
            cy = row_cy - 6
            draw_condition_icon(draw, cond, intensity, cx, cy)
            draw.text(((col + 1) * CELL + CELL // 2, row_cy + CELL // 2 - 10),
                       intensity, fill=LABEL_COLOR, font=font, anchor="mm")
    y_cursor += variant_rows * CELL

    draw.text((10, y_cursor + header_h // 2), "Sunrise / sunset status icons",
               fill=LABEL_COLOR, font=header_font, anchor="lm")
    y_cursor += header_h

    draw_sun_event(draw, 1 * CELL + 20, y_cursor + CELL // 2, True, FOG)
    draw.text((1 * CELL + CELL // 2, y_cursor + CELL - 14), "sunrise",
               fill=LABEL_COLOR, font=font, anchor="mm")
    draw_sun_event(draw, 3 * CELL + 20, y_cursor + CELL // 2, False, FOG)
    draw.text((3 * CELL + CELL // 2, y_cursor + CELL - 14), "sunset",
               fill=LABEL_COLOR, font=font, anchor="mm")

    img.save(OUT_PATH)
    print(f"Wrote {OUT_PATH}")


if __name__ == "__main__":
    main()

