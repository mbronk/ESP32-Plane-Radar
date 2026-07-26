#include "ui/stats_display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <cstdio>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/weather.h"

namespace lgfx_fonts = lgfx::v1::fonts;

namespace ui {

namespace {

constexpr int kCx = config::kDisplayWidth / 2;
constexpr uint16_t kColorGood = 0x7E8B;   // greenish
constexpr uint16_t kColorWarn = 0xFE60;   // yellowish
constexpr uint16_t kColorBad = 0xF8A5;    // reddish

uint16_t signalColor(int rssi_dbm) {
  if (rssi_dbm >= -60) {
    return kColorGood;
  }
  if (rssi_dbm >= -75) {
    return kColorWarn;
  }
  return kColorBad;
}

void setStatsFont() {
  if (displayFontIsSmooth() && displayFontEnsureLoaded(tft)) {
    displayFontSetSmoothSize(tft, 0.78f);
  } else {
    displayFontSetBitmap(tft, &lgfx_fonts::FreeSans9pt7b);
  }
}

void formatUptime(char* buf, size_t len) {
  unsigned long secs = millis() / 1000;
  const unsigned long days = secs / 86400;
  secs %= 86400;
  const unsigned long hours = secs / 3600;
  secs %= 3600;
  const unsigned long mins = secs / 60;
  secs %= 60;

  if (days > 0) {
    std::snprintf(buf, len, "%lud %luh %lum", days, hours, mins);
  } else if (hours > 0) {
    std::snprintf(buf, len, "%luh %lum", hours, mins);
  } else {
    std::snprintf(buf, len, "%lum %lus", mins, secs);
  }
}

}  // namespace

void statsDisplayDraw() {
  displayFontEnsureLoaded(tft);
  const uint16_t bg = config::kColorBlack;
  const uint16_t fg = config::kTextOnBlack;
  tft.fillScreen(bg);

  setStatsFont();
  const int line_h = tft.fontHeight() + 4;

  constexpr int kMaxLines = 9;
  char lines[kMaxLines][40];
  uint16_t line_colors[kMaxLines];
  int n = 0;

  std::snprintf(lines[n], sizeof(lines[n]), "Diagnostyka");
  line_colors[n] = fg;
  ++n;

  const bool wifi_up = WiFi.status() == WL_CONNECTED;
  if (wifi_up) {
    std::snprintf(lines[n], sizeof(lines[n]), "%s", WiFi.SSID().c_str());
    line_colors[n] = fg;
    ++n;

    const int rssi = WiFi.RSSI();
    std::snprintf(lines[n], sizeof(lines[n]), "Sygnal: %d dBm", rssi);
    line_colors[n] = signalColor(rssi);
    ++n;

    std::snprintf(lines[n], sizeof(lines[n]), "IP: %s",
                  WiFi.localIP().toString().c_str());
    line_colors[n] = fg;
    ++n;
  } else {
    std::snprintf(lines[n], sizeof(lines[n]), "WiFi: rozlaczony");
    line_colors[n] = kColorBad;
    ++n;
  }

  const services::weather::Data& w = services::weather::current();
  if (w.location[0] != '\0') {
    if (w.elevation_m >= 0.0f) {
      std::snprintf(lines[n], sizeof(lines[n]), "%s, %.0fm n.p.m.", w.location,
                    w.elevation_m);
    } else {
      std::snprintf(lines[n], sizeof(lines[n]), "%s", w.location);
    }
    line_colors[n] = fg;
    ++n;
  }

  std::snprintf(lines[n], sizeof(lines[n]), "%.4f, %.4f",
                services::location::lat(), services::location::lon());
  line_colors[n] = fg;
  ++n;

  if (w.valid && w.datetime[0][0] != '\0') {
    std::snprintf(lines[n], sizeof(lines[n]), "Czas: %s", w.datetime[0]);
    line_colors[n] = fg;
    ++n;
  }

  char uptime_buf[24];
  formatUptime(uptime_buf, sizeof(uptime_buf));
  std::snprintf(lines[n], sizeof(lines[n]), "Uptime: %s", uptime_buf);
  line_colors[n] = fg;
  ++n;

  std::snprintf(lines[n], sizeof(lines[n]), "Pamiec: %u KB  Samoloty: %u",
                static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                static_cast<unsigned>(services::adsb::aircraftCount()));
  line_colors[n] = fg;
  ++n;

  tft.setTextDatum(textdatum_t::middle_center);
  const int total_h = n * line_h;
  int y = (config::kDisplayHeight - total_h) / 2 + line_h / 2;
  for (int i = 0; i < n; ++i) {
    tft.setTextColor(line_colors[i], bg);
    tft.drawString(lines[i], kCx, y);
    y += line_h;
  }
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace ui
