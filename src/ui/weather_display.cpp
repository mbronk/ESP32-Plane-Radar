#include "ui/weather_display.h"

#include <WiFi.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/weather.h"
#include "services/radar_location.h"
#include "ui/radar_theme.h"

namespace lgfx_fonts = lgfx::v1::fonts;

namespace ui {

using services::weather::Condition;
using services::weather::Intensity;

constexpr int kCx = config::kDisplayWidth / 2;
uint8_t s_forecast_idx = 0;
bool s_auto_cycle_enabled = false;
// Not a timestamp -- counts completed footer rotation cycles (see
// kFooterModeIntervalMs/kFooterModeCount below) so the auto-cycle range
// switch always lands right as the 4-info footer rotation would repeat.
unsigned long s_auto_cycle_last_cycle_idx = 0;
bool s_auto_cycle_armed = false;

// The footer alternates through this many info lines, each shown for this
// long -- drawWeatherFooter() below derives footer_mode from the same
// constants. Auto-cycle advances the forecast range once every full
// rotation (kFooterModeCount * kFooterModeIntervalMs), i.e. just before the
// first info would repeat.
constexpr unsigned long kFooterModeIntervalMs = 5000UL;
constexpr int kFooterModeCount = 4;
constexpr unsigned long kAutoCycleIntervalMs =
    kFooterModeIntervalMs * kFooterModeCount;

namespace {

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (config::kDisplayRgbOrder) {
    return tft.color565(b, g, r);
  }
  return tft.color565(r, g, b);
}

uint16_t bgColor() {
  return tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
}
uint16_t textColor() { return rgb(255, 255, 255); }

// Footer/status text now uses the Polish-capable smooth VLW font when it's
// loaded (native size — matches the visible size of the FreeSans9pt7b bitmap
// font it replaces), falling back to the bitmap font if the VLW font failed
// to load.
void setFooterFont() {
  if (displayFontIsSmooth() && displayFontEnsureLoaded(tft)) {
    displayFontSetSmoothSize(tft, 1.0f);
  } else {
    displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
  }
}

void drawSun(int cx, int cy, int r, uint16_t color) {
  for (int a = 0; a < 360; a += 45) {
    const float rad = a * 0.01745329252f;
    const int x1 = cx + static_cast<int>(std::cos(rad) * (r + 4));
    const int y1 = cy + static_cast<int>(std::sin(rad) * (r + 4));
    const int x2 = cx + static_cast<int>(std::cos(rad) * (r + 11));
    const int y2 = cy + static_cast<int>(std::sin(rad) * (r + 11));
    tft.drawWideLine(x1, y1, x2, y2, 1.5f, color);
  }
  tft.fillCircle(cx, cy, r, color);
}

void drawCloud(int cx, int cy, uint16_t color) {
  // Flat-bottomed body drawn as a single rounded rect (gently rounded
  // corners, not full-height circles) -- the previous side-circle approach
  // made the base circles' own rounded "shoulders" visible as two
  // protruding bumps flanking the flat rect, looking like a cloud on
  // wheels. A rounded rect has no such seam to misjudge.
  const int body_w = 52;
  const int body_h = 22;
  const int body_r = 8;
  tft.fillRoundRect(cx - body_w / 2, cy - 4, body_w, body_h, body_r, color);

  // Puffy, unevenly-sized top circles, overlapping generously into the body
  // so the silhouette reads as an actual cloud rather than a plain slab.
  tft.fillCircle(cx - 10, cy - 5, 12, color);
  tft.fillCircle(cx + 10, cy - 6, 13, color);
  tft.fillCircle(cx + 1, cy - 12, 12, color);
}

// Fills xs[] (up to 4 entries) with x-offsets evenly spaced `spacing` apart
// around cx, and returns how many to draw: Light -> 2, Moderate -> 3,
// Heavy -> 4. A single mark looked odd, so even "Light" always shows at
// least two; more/wider-spread marks read as increasing intensity.
int intensitySlots(int cx, int spacing, Intensity intensity, int* xs) {
  const int n = intensity == Intensity::Light ? 2 :
                (intensity == Intensity::Heavy ? 4 : 3);
  for (int i = 0; i < n; ++i) {
    const float offset = spacing * (i - (n - 1) / 2.0f);
    xs[i] = cx + static_cast<int>(std::round(offset));
  }
  return n;
}

void drawRaindrops(int cx, int cy, uint16_t color, Intensity intensity) {
  int xs[4];
  const int n = intensitySlots(cx, 12, intensity, xs);
  const float thick = intensity == Intensity::Heavy ? 2.0f : 1.5f;
  for (int i = 0; i < n; ++i) {
    tft.drawWideLine(xs[i], cy, xs[i] - 4, cy + 12, thick, color);
  }
}

// Drizzle is much lighter than rain, so it's drawn as small falling dots
// rather than long diagonal streaks -- a clearly different shape, not just
// a smaller/fewer version of the rain icon.
void drawDrizzleDots(int cx, int cy, uint16_t color, Intensity intensity) {
  int xs[4];
  const int n = intensitySlots(cx, 12, intensity, xs);
  for (int i = 0; i < n; ++i) {
    tft.fillCircle(xs[i], cy, 2, color);
    tft.fillCircle(xs[i], cy + 8, 1, color);
  }
}

void drawSnowflakes(int cx, int cy, uint16_t color, Intensity intensity) {
  // A small 3-axis asterisk reads as "snow" much better than a plain dot.
  int xs[4];
  const int n = intensitySlots(cx, 13, intensity, xs);
  for (int i = 0; i < n; ++i) {
    const int x = xs[i];
    const int y = cy + 6;
    tft.drawWideLine(x - 4, y, x + 4, y, 1.2f, color);
    tft.drawWideLine(x - 3, y - 3, x + 3, y + 3, 1.2f, color);
    tft.drawWideLine(x - 3, y + 3, x + 3, y - 3, 1.2f, color);
  }
}

// Icy raindrops for freezing rain/drizzle: a short raindrop stem topped by a
// small frozen bead, distinguishing it from plain rain's plain diagonal drops.
void drawIcePellets(int cx, int cy, uint16_t color, Intensity intensity) {
  int xs[4];
  const int n = intensitySlots(cx, 12, intensity, xs);
  for (int i = 0; i < n; ++i) {
    tft.drawWideLine(xs[i], cy, xs[i], cy + 10, 1.5f, color);
    tft.fillCircle(xs[i], cy + 12, 2, color);
  }
}

// Small round hail pellets, drawn alongside a lightning bolt to distinguish
// hail-bearing thunderstorms from plain storms.
void drawHailPellets(int cx, int cy, uint16_t color, Intensity intensity) {
  int xs[4];
  const int n = intensitySlots(cx, 11, intensity, xs);
  for (int i = 0; i < n; ++i) {
    tft.fillCircle(xs[i], cy, 3, color);
  }
}

void drawBolt(int cx, int cy, uint16_t color) {
  tft.fillTriangle(cx - 5, cy, cx + 6, cy, cx - 2, cy + 14, color);
  tft.fillTriangle(cx + 3, cy + 8, cx + 11, cy + 8, cx - 3, cy + 26, color);
}

void drawWaterDrop(int cx, int cy, uint16_t color) {
  tft.fillTriangle(cx, cy - 8, cx - 6, cy + 2, cx + 6, cy + 2, color);
  tft.fillCircle(cx, cy + 4, 5, color);
}

void drawThermometerIcon(int cx, int cy, uint16_t color) {
  // Small hollow (outline-only) thermometer -- sized and weighted to sit
  // inline with the footer text rather than towering over it.
  const int stem_w = 4;
  const int bulb_r = 4;
  const int stem_h = 7;
  const int total_h = stem_h + bulb_r * 2;
  const int top = cy - total_h / 2;
  const int bulb_cy = top + total_h - bulb_r;
  tft.drawRoundRect(cx - stem_w / 2, top, stem_w, stem_h + bulb_r, stem_w / 2, color);
  tft.drawCircle(cx, bulb_cy, bulb_r, color);
  tft.fillCircle(cx, bulb_cy, 1, color);
}

void drawPrecipIcon(int cx, int cy, uint16_t color) {
  for (int i = -2; i <= 1; ++i) {
    const int dx = i * 5;
    const int x1 = cx + dx;
    const int y1 = cy - 6;
    const int x2 = cx + dx + 4;
    const int y2 = cy - 1;
    tft.drawWideLine(x1, y1, x2, y2, 1.5f, color);
  }
}

void drawWindDirectionArrow(int cx, int cy, int deg, uint16_t color) {
  const float rad = (deg + 180.0f) * 0.01745329252f; // arrow points toward where the wind blows
  const int len = 12;
  const int x2 = cx + static_cast<int>(std::round(std::cos(rad) * len));
  const int y2 = cy + static_cast<int>(std::round(std::sin(rad) * len));
  tft.drawWideLine(cx, cy, x2, y2, 1.5f, color);
  const float head_len = 5.0f;
  const float left_rad = rad + 2.3f;
  const float right_rad = rad - 2.3f;
  tft.drawWideLine(x2, y2, x2 + static_cast<int>(std::round(std::cos(left_rad) * head_len)),
                   y2 + static_cast<int>(std::round(std::sin(left_rad) * head_len)),
                   1.5f, color);
  tft.drawWideLine(x2, y2, x2 + static_cast<int>(std::round(std::cos(right_rad) * head_len)),
                   y2 + static_cast<int>(std::round(std::sin(right_rad) * head_len)),
                   1.5f, color);
}

void drawArrowDown(int cx, int cy, uint16_t color) {
  tft.fillTriangle(cx, cy + 4, cx - 5, cy - 2, cx + 5, cy - 2, color);
}

void drawArrowUp(int cx, int cy, uint16_t color) {
  tft.fillTriangle(cx, cy - 4, cx - 5, cy + 2, cx + 5, cy + 2, color);
}

void drawSunEvent(int x, int y, bool sunrise, uint16_t color) {
  const int w = 14;
  const int r = 4;
  const int cx = x + w / 2;
  const int cy = y - 2;
  const int horizon = y;
  tft.drawLine(x, horizon, x + w, horizon, color);
  tft.fillCircle(cx, cy, r, color);
  if (sunrise) {
    for (int dx = -8; dx <= 8; dx += 8) {
      tft.drawWideLine(cx, cy, cx + dx, cy - 8, 1.2f, color);
    }
  } else {
    for (int dx = -6; dx <= 6; dx += 6) {
      tft.drawWideLine(cx, cy, cx + dx, cy + 8, 1.2f, color);
    }
  }
}

bool parseTimeHHMM(const char* text, int* out_minutes) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  int hours = 0;
  int mins = 0;
  if (std::sscanf(text, "%d:%d", &hours, &mins) != 2) {
    return false;
  }
  if (hours < 0 || hours > 23 || mins < 0 || mins > 59) {
    return false;
  }
  *out_minutes = hours * 60 + mins;
  return true;
}

void toAsciiString(const char* src, char* dst, size_t dst_len) {
  if (dst_len == 0) {
    return;
  }
  size_t pos = 0;
  for (size_t i = 0; src[i] != '\0' && pos + 1 < dst_len; ++i) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if (c < 0x80) {
      dst[pos++] = static_cast<char>(c);
      continue;
    }

    unsigned char next = static_cast<unsigned char>(src[i + 1]);
    if (c == 0xC4 && next == 0x84) { dst[pos++] = 'A'; i += 1; }
    else if (c == 0xC4 && next == 0x85) { dst[pos++] = 'a'; i += 1; }
    else if (c == 0xC4 && next == 0x86) { dst[pos++] = 'C'; i += 1; }
    else if (c == 0xC4 && next == 0x87) { dst[pos++] = 'c'; i += 1; }
    else if (c == 0xC4 && next == 0x98) { dst[pos++] = 'E'; i += 1; }
    else if (c == 0xC4 && next == 0x99) { dst[pos++] = 'e'; i += 1; }
    else if (c == 0xC5 && next == 0x81) { dst[pos++] = 'L'; i += 1; }
    else if (c == 0xC5 && next == 0x82) { dst[pos++] = 'l'; i += 1; }
    else if (c == 0xC5 && next == 0x83) { dst[pos++] = 'N'; i += 1; }
    else if (c == 0xC5 && next == 0x84) { dst[pos++] = 'n'; i += 1; }
    else if (c == 0xC3 && next == 0x93) { dst[pos++] = 'O'; i += 1; }
    else if (c == 0xC3 && next == 0xB3) { dst[pos++] = 'o'; i += 1; }
    else if (c == 0xC5 && next == 0x9A) { dst[pos++] = 'S'; i += 1; }
    else if (c == 0xC5 && next == 0x9B) { dst[pos++] = 's'; i += 1; }
    else if (c == 0xC5 && next == 0xB9) { dst[pos++] = 'Z'; i += 1; }
    else if (c == 0xC5 && next == 0xBA) { dst[pos++] = 'z'; i += 1; }
    else if (c == 0xC5 && next == 0xBB) { dst[pos++] = 'Z'; i += 1; }
    else if (c == 0xC5 && next == 0xBC) { dst[pos++] = 'z'; i += 1; }
    // Skip unsupported UTF-8 bytes.
  }
  dst[pos] = '\0';
}

// Returns true if every character in src is either plain ASCII or one of the
// Polish Latin Extended-A letters present in the embedded smooth VLW font, so
// it can be rendered as-is instead of falling back to toAsciiString().
bool isFontSupportedUtf8(const char* src) {
  if (src == nullptr) {
    return true;
  }
  for (size_t i = 0; src[i] != '\0';) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    uint32_t codepoint;
    size_t len;
    if (c < 0x80) {
      codepoint = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      const unsigned char c1 = static_cast<unsigned char>(src[i + 1]);
      if ((c1 & 0xC0) != 0x80) {
        return false;
      }
      codepoint = (static_cast<uint32_t>(c & 0x1F) << 6) | (c1 & 0x3F);
      len = 2;
    } else {
      return false;  // 3+ byte UTF-8 sequences aren't in our glyph set.
    }
    i += len;

    if (codepoint >= 0x20 && codepoint <= 0x7E) {
      continue;
    }
    switch (codepoint) {
      case 0x104: case 0x105: case 0x106: case 0x107:
      case 0x118: case 0x119: case 0x141: case 0x142:
      case 0x143: case 0x144: case 0x15A: case 0x15B:
      case 0x179: case 0x17A: case 0x17B: case 0x17C:
      case 0xD3: case 0xF3:
        continue;
      default:
        return false;
    }
  }
  return true;
}

void drawPressureLine(int cx, int y, float pressure_hpa, uint16_t color) {
  displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
  tft.setTextColor(color, bgColor());
  tft.setTextDatum(textdatum_t::middle_center);

  char text[32];
  std::snprintf(text, sizeof(text), "Ci\xc5\x9bnienie %.0f hPa", pressure_hpa);
  tft.drawString(text, cx, y);
}

const char* windDirectionLabel(int deg) {
  static const char* kDirs[] = {"N", "NNE", "NE", "ENE", "E", "ESE",
                                "SE", "SSE", "S", "SSW", "SW", "WSW",
                                "W", "WNW", "NW", "NNW"};
  if (deg < 0) {
    return "?";
  }
  deg = deg % 360;
  if (deg < 0) {
    deg += 360;
  }
  const int index = static_cast<int>((deg + 11.25f) / 22.5f) % 16;
  return kDirs[index];
}

// WHO UV Index scale: 0-2 low, 3-5 moderate, 6-7 high, 8-10 very high,
// 11+ extreme.
uint16_t uvColor(float uv) {
  if (uv < 3.0f) return rgb(120, 200, 120);   // low - green
  if (uv < 6.0f) return rgb(230, 200, 60);    // moderate - yellow
  if (uv < 8.0f) return rgb(230, 140, 60);    // high - orange
  if (uv < 11.0f) return rgb(220, 70, 70);    // very high - red
  return rgb(170, 80, 190);                   // extreme - purple
}

// Open-Meteo/EEA European AQI scale: 0-20 good, 20-40 fair, 40-60 moderate,
// 60-80 poor, 80-100 very poor, 100+ extremely poor.
uint16_t aqiColor(float aqi) {
  if (aqi < 20.0f) return rgb(100, 200, 120);  // good - green
  if (aqi < 40.0f) return rgb(190, 210, 90);   // fair - yellow-green
  if (aqi < 60.0f) return rgb(230, 190, 60);   // moderate - yellow/orange
  if (aqi < 80.0f) return rgb(230, 130, 60);   // poor - orange
  if (aqi < 100.0f) return rgb(220, 70, 70);   // very poor - red
  return rgb(160, 60, 140);                    // extremely poor - purple
}

// Beaufort-inspired wind speed color scale (km/h). Calm-to-strong-breeze
// (Bft 0-5) stays neutral/white; only noticeably strong wind gets flagged,
// escalating from yellow to orange to red. Used for both sustained speed
// and gust independently.
uint16_t windColor(float kmh) {
  if (kmh < 39.0f) return rgb(180, 190, 205);  // calm..strong breeze (Bft 0-5)
  if (kmh < 62.0f) return rgb(230, 200, 60);   // near gale/gale (Bft 6-7)
  if (kmh < 89.0f) return rgb(230, 140, 60);   // severe gale/storm (Bft 8-9)
  return rgb(220, 70, 70);                     // violent storm+ (Bft 10+)
}

// Super-short Polish labels so "UV .. / AQI .. (grade)" still fits on one
// footer line.
const char* aqiGradeLabel(float aqi) {
  if (aqi < 20.0f) return "czyste";
  if (aqi < 40.0f) return "dobre";
  if (aqi < 60.0f) return "\xc5\x9br.";              // "śr." (średnie)
  if (aqi < 80.0f) return "z\xc5\x82" "e";            // "złe"
  if (aqi < 100.0f) return "b.z\xc5\x82" "e";         // "b.złe"
  return "skr.z\xc5\x82" "e";                         // "skr.złe"
}

void drawTempLineLeft(int x, int y, float value_c,
                      const lgfx::GFXfont* font, uint16_t color,
                      const char* prefix, int right_edge) {
  displayFontSetBitmap(tft, font);
  tft.setTextColor(color, bgColor());

  char num[8];
  std::snprintf(num, sizeof(num), "%d",
                static_cast<int>(std::lround(value_c)));
  const int h = tft.fontHeight();
  const bool has_prefix = (prefix != nullptr && prefix[0] != '\0');
  const int prefix_w = has_prefix ? tft.textWidth(prefix) + 6 : 0;
  const int num_w = tft.textWidth(num);
  const int ring_r = std::max(2, h / 9);
  const int deg_w = tft.textWidth("C");
  const int total_w = prefix_w + num_w + ring_r * 2 + 2 + deg_w;
  const int top = y - h / 2;
  int cur_x = right_edge - total_w;

  tft.setTextDatum(textdatum_t::top_left);
  if (has_prefix) {
    tft.drawString(prefix, cur_x, top);
    cur_x += prefix_w;
  }

  tft.drawString(num, cur_x, top);
  cur_x += num_w + 2;

  const int ring_cx = cur_x + ring_r;
  const int ring_cy = top + ring_r + 1;
  tft.drawCircle(ring_cx, ring_cy, ring_r, color);
  if (ring_r >= 3) {
    tft.drawCircle(ring_cx, ring_cy, ring_r - 1, color);
  }
  cur_x += ring_r * 2 + 2;

  tft.drawString("C", cur_x, top);
}

// Measures (without drawing) the total width drawTempRightHumidPrecip would
// occupy, so the alternating big-temp line can be positioned to never
// collide with it regardless of which footer mode is currently showing.
int humidPrecipWidth(float humidity_pct, float precip_pct) {
  setFooterFont();
  int humid_w = 0;
  int precip_w = 0;
  if (humidity_pct >= 0.0f) {
    char humid_text[8];
    std::snprintf(humid_text, sizeof(humid_text), "%d",
                  static_cast<int>(std::lround(humidity_pct)));
    humid_w = tft.textWidth(humid_text) + 16;
  }
  if (precip_pct >= 0.0f) {
    char precip_text[8];
    std::snprintf(precip_text, sizeof(precip_text), "%d%%",
                  static_cast<int>(std::lround(precip_pct)));
    precip_w = tft.textWidth(precip_text) + 24;
  }
  return humid_w + precip_w;
}

void drawTempRightHumidPrecip(int y, float humidity_pct, float precip_pct) {
  const int h = tft.fontHeight();
  const int top = y - h / 2;
  char humid_text[8] = {0};
  char precip_text[8] = {0};
  int humid_w = 0;
  int precip_w = 0;

  setFooterFont();
  if (humidity_pct >= 0.0f) {
    std::snprintf(humid_text, sizeof(humid_text), "%d",
                  static_cast<int>(std::lround(humidity_pct)));
    humid_w = tft.textWidth(humid_text) + 16;
  }
  if (precip_pct >= 0.0f) {
    std::snprintf(precip_text, sizeof(precip_text), "%d%%",
                  static_cast<int>(std::lround(precip_pct)));
    precip_w = tft.textWidth(precip_text) + 24;
  }

  const int total_w = humid_w + precip_w;
  int x = config::kDisplayWidth - 10 - total_w;

  if (humid_w > 0) {
    const int drop_cx = x + 5;
    const int drop_cy = top + h / 2 + 2;
    drawWaterDrop(drop_cx, drop_cy, rgb(80, 180, 255));
    x += 14;
    tft.setTextColor(rgb(80, 180, 255), bgColor());
    tft.drawString(humid_text, x, top + 2);
    x += tft.textWidth(humid_text) + 8;
    tft.setTextColor(textColor(), bgColor());
  }

  if (precip_w > 0) {
    const int icon_cx = x + 8;
    const int icon_cy = top + h / 2 + 1;
    const uint16_t prob_color = (precip_pct > 0.0f)
                                    ? rgb(180, 160, 255)
                                    : rgb(120, 120, 120);
    drawPrecipIcon(icon_cx, icon_cy, prob_color);
    x += 22;
    tft.setTextColor(prob_color, bgColor());
    tft.drawString(precip_text, x, top + 2);
  }
}

// Measures (without drawing) the total width drawTempRightMinMax would
// occupy, mirroring the layout math below.
int minMaxWidth(float temp_min_c, float temp_max_c) {
  setFooterFont();
  if (temp_min_c >= -100.0f && temp_max_c >= -100.0f) {
    char min_text[8];
    char max_text[8];
    std::snprintf(min_text, sizeof(min_text), "%d",
                  static_cast<int>(std::lround(temp_min_c)));
    std::snprintf(max_text, sizeof(max_text), "%d",
                  static_cast<int>(std::lround(temp_max_c)));
    const int min_w = tft.textWidth(min_text);
    const int max_w = tft.textWidth(max_text);
    const int block_gap = 10;
    const int arrow_w = 10;
    return arrow_w + min_w + block_gap + arrow_w + max_w;
  }
  return tft.textWidth("--/--");
}

void drawTempRightMinMax(int y, float temp_min_c, float temp_max_c) {
  const int h = tft.fontHeight();
  const int top = y - h / 2;
  setFooterFont();
  tft.setTextColor(rgb(180, 190, 205), bgColor());

  if (temp_min_c >= -100.0f && temp_max_c >= -100.0f) {
    char min_text[8];
    char max_text[8];
    std::snprintf(min_text, sizeof(min_text), "%d",
                  static_cast<int>(std::lround(temp_min_c)));
    std::snprintf(max_text, sizeof(max_text), "%d",
                  static_cast<int>(std::lround(temp_max_c)));

    const int min_w = tft.textWidth(min_text);
    const int max_w = tft.textWidth(max_text);
    const int block_gap = 10;
    const int arrow_w = 10;
    const int total_w = arrow_w + min_w + block_gap + arrow_w + max_w;
    int x = config::kDisplayWidth - 10 - total_w;
    const int cy = top + h / 2 + 2;

    drawArrowDown(x + 5, cy, rgb(90, 160, 255));
    x += arrow_w;
    tft.drawString(min_text, x, top + 2);
    x += min_w + block_gap;
    drawArrowUp(x + 5, cy, rgb(255, 90, 90));
    x += arrow_w;
    tft.drawString(max_text, x, top + 2);
  } else {
    tft.drawString("--/--", config::kDisplayWidth - 10 - tft.textWidth("--/--"), top + 2);
  }
}

void drawTempTopLine(const services::weather::Data& w, int mode) {
  const int y = 156;
  // The right-hand side alternates between two different layouts (humidity
  // /precip vs. min/max), each with its own data-dependent width. Compute
  // the leftmost edge either one could reach and let the big temp shift
  // right to just short of it, so it never overlaps whichever is showing.
  const int humid_precip_left =
      config::kDisplayWidth - 10 -
      humidPrecipWidth(w.humidity[s_forecast_idx], w.precip_pct[s_forecast_idx]);
  const int min_max_left =
      config::kDisplayWidth - 10 -
      minMaxWidth(w.temp_min_c[s_forecast_idx], w.temp_max_c[s_forecast_idx]);
  const int right_content_left = std::min(humid_precip_left, min_max_left);
  const int kGap = 14;
  const int kMinRightEdge = kCx - 10;   // never move further left than before
  const int kMaxRightEdge = kCx + 30;   // cap how far right it can drift
  int right_edge = right_content_left - kGap;
  right_edge = std::max(right_edge, kMinRightEdge);
  right_edge = std::min(right_edge, kMaxRightEdge);

  drawTempLineLeft(10, y, w.temp_c[s_forecast_idx],
                   &lgfx_fonts::FreeSansBold18pt7b, textColor(), nullptr,
                   right_edge);
  if (mode == 0) {
    drawTempRightHumidPrecip(y, w.humidity[s_forecast_idx],
                             w.precip_pct[s_forecast_idx]);
  } else {
    drawTempRightMinMax(y, w.temp_min_c[s_forecast_idx],
                        w.temp_max_c[s_forecast_idx]);
  }
}

void drawFooterLine2(const services::weather::Data& w, int mode) {
  char text[64] = {0};
  const float pressure = w.pressure_hpa[s_forecast_idx];
  const float feels = w.feels_c[s_forecast_idx];
  const float wind_speed = w.wind_speed_kmh[s_forecast_idx];
  const float wind_gust = w.wind_gust_kmh[s_forecast_idx];
  const int wind_dir = w.wind_dir_deg[s_forecast_idx];
  const float uv = w.uv_index_max[s_forecast_idx];
  const float aqi = w.aqi_european[s_forecast_idx];

  switch (mode) {
    case 0:
      // Plain text label -- was an icon + text layout using
      // drawThermometerIcon(), kept defined below in case this needs to
      // revert to the icon-based look.
      if (feels >= -100.0f) {
        std::snprintf(text, sizeof(text), "Temp. odczuwalna: %.0f\xc2\xb0" "C", feels);
      } else {
        std::snprintf(text, sizeof(text), "Temp. odczuwalna: --");
      }
      break;
    case 1:
      if (pressure >= 0.0f) {
        std::snprintf(text, sizeof(text), "Ci\xc5\x9bnienie %.0f hPa", pressure);
      } else {
        std::snprintf(text, sizeof(text), "Ci\xc5\x9bnienie --");
      }
      break;
    case 2: {
      const bool have_dir = wind_dir >= 0;
      const bool have_gust = wind_gust >= 0.0f;

      if (wind_speed >= 0.0f && have_dir) {
        // "Wiatr <direction icon> <speed> km/h (<up-triangle icon><gust>)"
        char prefix_text[16];
        std::snprintf(prefix_text, sizeof(prefix_text), "Wiatr");
        char main_text[16];
        std::snprintf(main_text, sizeof(main_text), "%.0f km/h", wind_speed);
        char gust_text[16] = {0};
        if (have_gust) {
          std::snprintf(gust_text, sizeof(gust_text), "%.0f", wind_gust);
        }

        const int dir_icon_w = 14;
        const int gust_icon_w = 10;
        const int spacer = 6;
        const int icon_gap = 2;
        const uint16_t color = rgb(180, 190, 205);
        const uint16_t speed_color = windColor(wind_speed);
        const uint16_t gust_color = have_gust ? windColor(wind_gust) : color;

        setFooterFont();
        tft.setTextColor(color, bgColor());
        const int prefix_w = tft.textWidth(prefix_text);
        const int main_w = tft.textWidth(main_text);
        const int main_h = tft.fontHeight();
        const int paren_open_w = tft.textWidth("(");
        const int paren_close_w = tft.textWidth(")");
        const int gust_num_w = have_gust ? tft.textWidth(gust_text) : 0;

        int total_w = prefix_w + spacer + dir_icon_w + icon_gap + main_w;
        if (have_gust) {
          total_w += spacer + paren_open_w + gust_icon_w + gust_num_w +
                     paren_close_w;
        }

        const int cy = 196;
        const int text_y = cy - main_h / 2;
        int x = kCx - total_w / 2;

        tft.setTextDatum(textdatum_t::top_left);
        tft.drawString(prefix_text, x, text_y);
        x += prefix_w + spacer;
        drawWindDirectionArrow(x + dir_icon_w / 2 - 2, cy - 3, wind_dir, color);
        x += dir_icon_w + icon_gap;
        tft.setTextColor(speed_color, bgColor());
        tft.drawString(main_text, x, text_y);
        x += main_w;


        if (have_gust) {
          x += spacer;
          tft.setTextColor(color, bgColor());
          tft.drawString("(", x, text_y);
          x += paren_open_w;
          drawArrowUp(x + gust_icon_w / 2, cy, color);
          x += gust_icon_w;
          tft.setTextColor(gust_color, bgColor());
          tft.drawString(gust_text, x, text_y);
          x += gust_num_w;
          tft.setTextColor(color, bgColor());
          tft.drawString(")", x, text_y);
        }
        return;
      }

      if (wind_speed >= 0.0f) {
        if (have_gust) {
          std::snprintf(text, sizeof(text), "Wiatr %.0f km/h (%.0f)",
                        wind_speed, wind_gust);
        } else {
          std::snprintf(text, sizeof(text), "Wiatr %.0f km/h", wind_speed);
        }
      } else {
        std::snprintf(text, sizeof(text), "Wiatr --");
      }
    } break;
    case 3: {
      // "UV <value> / AQI <value> (grade)" -- both halves color-coded by
      // severity; the AQI half is only drawn when that (optional, separate)
      // fetch succeeded.
      char uv_text[16];
      if (uv >= 0.0f) {
        std::snprintf(uv_text, sizeof(uv_text), "UV %d",
                      static_cast<int>(std::lround(uv)));
      } else {
        std::snprintf(uv_text, sizeof(uv_text), "UV --");
      }
      const uint16_t uv_color = uv >= 0.0f ? uvColor(uv) : rgb(180, 190, 205);
      const uint16_t muted = rgb(180, 190, 205);

      const bool have_aqi = aqi >= 0.0f;
      char aqi_num_text[8] = {0};
      char aqi_paren_text[16] = {0};
      uint16_t aqi_color = muted;
      if (have_aqi) {
        std::snprintf(aqi_num_text, sizeof(aqi_num_text), "%d",
                      static_cast<int>(std::lround(aqi)));
        std::snprintf(aqi_paren_text, sizeof(aqi_paren_text), " (%s)",
                      aqiGradeLabel(aqi));
        aqi_color = aqiColor(aqi);
      }
      static const char kSepText[] = " / AQI ";

      setFooterFont();
      const int h = tft.fontHeight();
      const int cy = 196;
      const int text_y = cy - h / 2;

      tft.setTextColor(uv_color, bgColor());
      const int uv_w = tft.textWidth(uv_text);

      int total_w = uv_w;
      int sep_w = 0;
      int aqi_num_w = 0;
      int aqi_paren_w = 0;
      if (have_aqi) {
        tft.setTextColor(muted, bgColor());
        sep_w = tft.textWidth(kSepText);
        tft.setTextColor(aqi_color, bgColor());
        aqi_num_w = tft.textWidth(aqi_num_text);
        aqi_paren_w = tft.textWidth(aqi_paren_text);
        total_w += sep_w + aqi_num_w + aqi_paren_w;
      }

      int x = kCx - total_w / 2;
      tft.setTextDatum(textdatum_t::top_left);
      tft.setTextColor(uv_color, bgColor());
      tft.drawString(uv_text, x, text_y);
      x += uv_w;

      if (have_aqi) {
        tft.setTextColor(muted, bgColor());
        tft.drawString(kSepText, x, text_y);
        x += sep_w;
        tft.setTextColor(aqi_color, bgColor());
        tft.drawString(aqi_num_text, x, text_y);
        x += aqi_num_w;
        tft.drawString(aqi_paren_text, x, text_y);
      }
      return;
    }
  }

  setFooterFont();
  tft.setTextColor(rgb(180, 190, 205), bgColor());
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, kCx, 196);
}

void drawFooterStatusLine(const services::weather::Data& w, int mode) {
  char text[80] = {0};

  switch (mode) {
    case 0:
      std::snprintf(text, sizeof(text), "%s", w.datetime[s_forecast_idx]);
      break;
    case 1: {
      if (w.location[0] != '\0') {
        if (isFontSupportedUtf8(w.location)) {
          std::snprintf(text, sizeof(text), "%s", w.location);
        } else {
          char ascii_location[32] = {0};
          toAsciiString(w.location, ascii_location, sizeof(ascii_location));
          std::snprintf(text, sizeof(text), "%s", ascii_location);
        }
      } else {
        const double lat = services::location::lat();
        const double lon = services::location::lon();
        const char lat_dir = lat >= 0 ? 'N' : 'S';
        const char lon_dir = lon >= 0 ? 'E' : 'W';
        const float elev = w.elevation_m;
        if (elev >= 0.0f) {
          std::snprintf(text, sizeof(text), "%02.1f%c %03.1f%c %dm",
                        std::abs(lat), lat_dir, std::abs(lon), lon_dir,
                        static_cast<int>(std::lround(elev)));
        } else {
          std::snprintf(text, sizeof(text), "%02.1f%c %03.1f%c",
                        std::abs(lat), lat_dir, std::abs(lon), lon_dir);
        }
      }
    } break;
    case 2: {
      const char* sunrise = w.sunrise[s_forecast_idx];
      const char* sunset = w.sunset[s_forecast_idx];
      int sunrise_min = 0;
      int sunset_min = 0;
      const bool have_sunrise = parseTimeHHMM(sunrise, &sunrise_min);
      const bool have_sunset = parseTimeHHMM(sunset, &sunset_min);

      if (have_sunrise && have_sunset) {
        const int icon_width = 14;
        const int spacer = 3;
        const int group_gap = 10;
        const uint16_t color = rgb(100, 100, 100);

        setFooterFont();
        tft.setTextColor(color, bgColor());
        const int sunrise_w = tft.textWidth(sunrise);
        const int sunset_w = tft.textWidth(sunset);
        const int total_w = icon_width + spacer + sunrise_w + group_gap +
                             icon_width + spacer + sunset_w;
        int x = kCx - total_w / 2;
        const int y = 218;
        const int text_y = y - tft.fontHeight() / 2;

        tft.setTextDatum(textdatum_t::top_left);
        drawSunEvent(x, y, /*sunrise=*/true, color);
        x += icon_width + spacer;
        tft.drawString(sunrise, x, text_y);
        x += sunrise_w + group_gap;

        drawSunEvent(x, y, /*sunrise=*/false, color);
        x += icon_width + spacer;
        tft.drawString(sunset, x, text_y);
        return;
      }
      std::snprintf(text, sizeof(text), "Wschód/Zachód --");
    } break;
    case 3:
      if (w.daylight_min[s_forecast_idx] >= 0) {
        const int mins = w.daylight_min[s_forecast_idx];
        const int hours = mins / 60;
        const int minutes = mins % 60;
        std::snprintf(text, sizeof(text), "D\xc5\x82. dnia: %dh%02d", hours,
                      minutes);
      } else {
        std::snprintf(text, sizeof(text), "D\xc5\x82. dnia: --");
      }
      break;
  }

  setFooterFont();
  tft.setTextColor(rgb(100, 100, 100), bgColor());
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(text, kCx, 218);
}

// TODO (do not pursue): consider reworking these icons to look more like
// AccuWeather's (e.g.
// https://www.accuweather.com/pl/pl/gdansk/275174/hourly-weather-forecast/275174),
// including day/night variants (sun vs. moon + stars behind clouds based on
// whether it's currently daytime). Left as a note only -- not planned work.
void drawIcon(Condition cond, Intensity intensity, int cx, int cy) {
  const uint16_t sun = rgb(255, 200, 0);
  const uint16_t cloud = rgb(205, 210, 220);
  const uint16_t cloud_dark = rgb(120, 132, 150);
  const uint16_t rain = rgb(70, 150, 255);
  const uint16_t snow = rgb(240, 245, 255);
  const uint16_t bolt = rgb(255, 215, 0);
  const uint16_t fog = rgb(180, 190, 200);

  switch (cond) {
    case Condition::Clear:
      drawSun(cx, cy, 20, sun);
      break;
    case Condition::PartlyCloudy:
      drawSun(cx + 12, cy - 12, 12, sun);
      drawCloud(cx - 2, cy + 6, cloud);
      break;
    case Condition::Cloudy:
      drawCloud(cx, cy + 2, cloud);
      break;
    case Condition::Fog:
      drawCloud(cx, cy - 6, fog);
      for (int i = 0; i < 3; ++i) {
        tft.drawWideLine(cx - 22, cy + 16 + i * 7, cx + 22,
                         cy + 16 + i * 7, 1.5f, fog);
      }
      break;
    case Condition::Drizzle:
      // Lighter cloud than Rain's (drizzle falls from thin low stratus, not
      // a dark rain cloud), plus small falling dots instead of streaks.
      drawCloud(cx, cy - 6, cloud);
      drawDrizzleDots(cx, cy + 14, rain, intensity);
      break;
    case Condition::Rain:
      drawCloud(cx, cy - 6, cloud_dark);
      drawRaindrops(cx, cy + 14, rain, intensity);
      break;
    case Condition::FreezingRain: {
      const uint16_t icy = rgb(180, 220, 255);
      drawCloud(cx, cy - 6, cloud_dark);
      drawIcePellets(cx, cy + 12, icy, intensity);
      break;
    }
    case Condition::Snow:
      drawCloud(cx, cy - 6, cloud);
      drawSnowflakes(cx, cy + 12, snow, intensity);
      break;
    case Condition::Storm:
      drawCloud(cx, cy - 6, cloud_dark);
      drawBolt(cx, cy + 12, bolt);
      break;
    case Condition::Hail: {
      const uint16_t hail_color = rgb(225, 230, 240);
      drawCloud(cx, cy - 6, cloud_dark);
      drawBolt(cx - 8, cy + 10, bolt);
      drawHailPellets(cx + 10, cy + 20, hail_color, intensity);
      break;
    }
    case Condition::Unknown:
    default:
      tft.drawCircle(cx, cy, 18, fog);
      break;
  }
}

void drawTempLine(int cx, int y, float value_c, float humidity_pct,
                  float precip_pct, const lgfx::GFXfont* font,
                  uint16_t color, const char* prefix) {
  displayFontSetBitmap(tft, font);
  tft.setTextColor(color, bgColor());

  char num[8];
  std::snprintf(num, sizeof(num), "%d",
                static_cast<int>(std::lround(value_c)));

  const int h = tft.fontHeight();
  const int ring_r = std::max(2, h / 9);
  constexpr int kGapNumDeg = 2;
  constexpr int kGapDegF = 1;
  constexpr int kGapPrefix = 6;
  constexpr int kGapHumid = 6;
  constexpr int kGapPrecip = 10;

  const bool has_prefix = (prefix != nullptr && prefix[0] != '\0');
  const int prefix_w = has_prefix ? tft.textWidth(prefix) + kGapPrefix : 0;
  const int num_w = tft.textWidth(num);
  const int f_w = tft.textWidth("C");
  const int deg_w = kGapNumDeg + ring_r * 2 + kGapDegF;
  constexpr int kGapTempHumid = 18;

  char humid_text[8] = {0};
  int humid_w = 0;
  if (humidity_pct >= 0.0f) {
    std::snprintf(humid_text, sizeof(humid_text), "%d",
                  static_cast<int>(std::lround(humidity_pct)));
    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 0.45f);
    } else {
      displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
    }
    humid_w = tft.textWidth(humid_text) + 8;
    displayFontSetBitmap(tft, font);
  }

  char precip_text[8] = {0};
  int precip_w = 0;
  if (precip_pct >= 0.0f) {
    std::snprintf(precip_text, sizeof(precip_text), "%d%%",
                  static_cast<int>(std::lround(precip_pct)));
    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 0.45f);
    } else {
      displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
    }
    precip_w = tft.textWidth(precip_text) + kGapPrecip;
    displayFontSetBitmap(tft, font);
  }

  const int total = prefix_w + num_w + deg_w + f_w + humid_w + precip_w + 12;

  tft.setTextDatum(textdatum_t::top_left);
  const int top = y - h / 2;
  int x = cx - total / 2;

  if (has_prefix) {
    tft.drawString(prefix, x, top);
    x += prefix_w;
  }
  tft.drawString(num, x, top);
  x += num_w + kGapNumDeg;

  const int ring_cx = x + ring_r;
  const int ring_cy = top + ring_r + 1;
  tft.drawCircle(ring_cx, ring_cy, ring_r, color);
  if (ring_r >= 3) {
    tft.drawCircle(ring_cx, ring_cy, ring_r - 1, color);
  }
  x += ring_r * 2 + kGapDegF;

  tft.drawString("C", x, top);
  x += f_w + kGapTempHumid;

  if (humidity_pct >= 0.0f) {
    const int drop_cx = x + 5;
    const int drop_cy = top + h / 2 + 2;
    drawWaterDrop(drop_cx, drop_cy, rgb(80, 180, 255));
    x += 14;

    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 0.45f);
    } else {
      displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
    }
    tft.setTextColor(rgb(80, 180, 255), bgColor());
    tft.drawString(humid_text, x, top + 2);
    x += humid_w;
    displayFontSetBitmap(tft, font);
    tft.setTextColor(color, bgColor());
  }

  if (precip_pct >= 0.0f) {
    const uint16_t prob_color = (precip_pct > 0.0f)
                                    ? rgb(180, 160, 255)
                                    : rgb(120, 120, 120);
    const int icon_cx = x + 8;
    const int icon_cy = top + h / 2 + 1;
    drawPrecipIcon(icon_cx, icon_cy, prob_color);
    x += 22;

    if (displayFontIsSmooth()) {
      displayFontSetSmoothSize(tft, 0.45f);
    } else {
      displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
    }
    tft.setTextColor(prob_color, bgColor());
    tft.drawString(precip_text, x, top + 2);
    displayFontSetBitmap(tft, font);
    tft.setTextColor(color, bgColor());
  }
}

}  // namespace

const char* kForecastLabels[4] = {"Teraz", "+2h", "+6h", "Jutro"};

void weatherDisplayAdvanceForecast() {
  s_forecast_idx = static_cast<uint8_t>((s_forecast_idx + 1) % 4);
}

void weatherDisplayResetForecast() {
  s_forecast_idx = 0;
}

void weatherDisplayToggleAutoCycle() {
  s_auto_cycle_enabled = !s_auto_cycle_enabled;
  // Re-arm: the next tick() call just records the footer's current rotation
  // cycle instead of advancing immediately, so the first switch still waits
  // for that cycle to finish rather than firing right away.
  s_auto_cycle_armed = false;
}

bool weatherDisplayAutoCycleEnabled() { return s_auto_cycle_enabled; }

bool weatherDisplayAutoCycleTick() {
  if (!s_auto_cycle_enabled) {
    return false;
  }
  const unsigned long cycle_idx = millis() / kAutoCycleIntervalMs;
  if (!s_auto_cycle_armed) {
    s_auto_cycle_armed = true;
    s_auto_cycle_last_cycle_idx = cycle_idx;
    return false;
  }
  if (cycle_idx == s_auto_cycle_last_cycle_idx) {
    return false;
  }
  s_auto_cycle_last_cycle_idx = cycle_idx;
  weatherDisplayAdvanceForecast();
  return true;
}

void drawWeatherHeader(const services::weather::Data& w) {
  displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);

  // Now (Teraz) stays the neutral theme color; the other slots get a
  // color hint so it's obvious you're not looking at "now": +2h/+6h use
  // shades of blue (still today), Jutro (tomorrow) uses a distinct orange.
  uint16_t label_color = textColor();
  switch (s_forecast_idx) {
    case 1: label_color = rgb(120, 190, 255); break;  // +2h - light blue
    case 2: label_color = rgb(40, 120, 230); break;   // +6h - deeper blue
    case 3: label_color = rgb(255, 170, 60); break;   // Jutro - orange
    default: break;                                   // Teraz - neutral
  }
  tft.setTextColor(label_color, bgColor());
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(kForecastLabels[s_forecast_idx], kCx, 12);

  if (s_auto_cycle_enabled) {
    tft.fillCircle(kCx, 8, 2, textColor());
  }

  drawIcon(w.condition[s_forecast_idx], w.intensity[s_forecast_idx], kCx, 70);

  // Condition label may contain Polish diacritics, so use the dedicated
  // header VLW font (rasterized natively at a larger size, drawn at scale
  // 1.0 — avoids the blur/blockiness that comes from upscaling the small
  // body font via a non-integer setTextSize()).
  if (displayFontHeaderIsSmooth() && displayFontHeaderEnsureLoaded(tft)) {
    displayFontSetSmoothSize(tft, 1.0f);
  } else {
    displayFontSetBitmap(tft, &lgfx_fonts::FreeSansBold12pt7b);
  }
  tft.setTextColor(textColor(), bgColor());
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(w.label[s_forecast_idx], kCx, 118);
}

void drawWeatherFooter(const services::weather::Data& w) {
  const uint8_t temp_mode = static_cast<uint8_t>((millis() / 8000) % 2);
  const uint8_t footer_mode = static_cast<uint8_t>(
      (millis() / kFooterModeIntervalMs) % kFooterModeCount);

  tft.fillRect(0, 140, config::kDisplayWidth, 100, bgColor());

  drawTempTopLine(w, temp_mode);
  drawFooterLine2(w, footer_mode);
  drawFooterStatusLine(w, footer_mode);
}

void weatherDisplayLoading() {
  displayFontEnsureLoaded(tft);
  const uint16_t bg = bgColor();
  tft.fillScreen(bg);

  displayFontSetBitmap(tft, &lgfx_fonts::FreeSansBold12pt7b);
  tft.setTextColor(textColor(), bg);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("Pogoda", kCx, 100);

  setFooterFont();
  tft.drawString(displayFontIsSmooth() ? "\xc5\x81" "aduje..." : "Laduje...",
                 kCx, 134);
  tft.setTextDatum(textdatum_t::top_left);
}

void weatherDisplayDraw() {
  displayFontEnsureLoaded(tft);
  const uint16_t bg = bgColor();
  tft.fillScreen(bg);

  const services::weather::Data& w = services::weather::current();

  if (!w.valid) {
    displayFontSetBitmap(tft, &lgfx_fonts::FreeSansBold12pt7b);
    tft.setTextColor(textColor(), bg);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString("Pogoda", kCx, 100);
    displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
    tft.drawString(WiFi.status() == WL_CONNECTED ? "Brak danych" : "Brak Wi-Fi",
                   kCx, 134);
    tft.setTextDatum(textdatum_t::top_left);
    return;
  }

  drawWeatherHeader(w);
  drawWeatherFooter(w);

  static unsigned long s_last_weather_log_ms = 0;
  if (millis() - s_last_weather_log_ms >= 10000) {
    s_last_weather_log_ms = millis();
    const char* bottom_text = w.location[0] != '\0' ? w.location :
        w.datetime[s_forecast_idx][0] != '\0' ?
            w.datetime[s_forecast_idx] : "(none)";
    const bool showPressure = (w.pressure_hpa[s_forecast_idx] >= 0.0f) &&
        ((millis() / 10000) % 2 == 1);
    Serial.printf("weather display idx=%u temp=%.0fC feels=%.0fC minmax=%.0f/%.0fC uv=%.1f hum=%.0f%% precip=%.0f%% pres=%.0fhPa wind=%.0fkm/h gust=%.0fkm/h dir=%d showPressure=%u bottom='%s'\n",
                  static_cast<unsigned>(s_forecast_idx),
                  w.temp_c[s_forecast_idx],
                  w.feels_c[s_forecast_idx],
                  w.temp_min_c[s_forecast_idx],
                  w.temp_max_c[s_forecast_idx],
                  w.uv_index_max[s_forecast_idx],
                  w.humidity[s_forecast_idx],
                  w.precip_pct[s_forecast_idx],
                  w.pressure_hpa[s_forecast_idx],
                  w.wind_speed_kmh[s_forecast_idx],
                  w.wind_gust_kmh[s_forecast_idx],
                  w.wind_dir_deg[s_forecast_idx],
                  static_cast<unsigned>(showPressure),
                  bottom_text);
    Serial.printf("weather slots minmax/uv: [0]%.0f/%.0f/%.1f [1]%.0f/%.0f/%.1f [2]%.0f/%.0f/%.1f [3]%.0f/%.0f/%.1f\n",
                  w.temp_min_c[0], w.temp_max_c[0], w.uv_index_max[0],
                  w.temp_min_c[1], w.temp_max_c[1], w.uv_index_max[1],
                  w.temp_min_c[2], w.temp_max_c[2], w.uv_index_max[2],
                  w.temp_min_c[3], w.temp_max_c[3], w.uv_index_max[3]);
  }

  drawWeatherFooter(w);
}

void weatherDisplayDrawPartial() {
  displayFontEnsureLoaded(tft);
  const services::weather::Data& w = services::weather::current();
  if (!w.valid) {
    return;
  }
  drawWeatherFooter(w);
}

}  // namespace ui
