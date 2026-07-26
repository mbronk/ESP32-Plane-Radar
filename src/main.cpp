/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "services/weather.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/stats_display.h"
#include "ui/status_screens.h"
#include "ui/weather_display.h"

namespace {

enum class Page { Radar, Weather, Stats };

bool g_radar_visible = false;
Page g_page = Page::Weather;
Page g_prev_page = Page::Weather;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_weather_fetch_ms = 0;
unsigned long g_last_weather_phase_4 = 0;
unsigned long g_last_weather_phase_8 = 0;
unsigned long g_last_stats_phase_4 = 0;
unsigned long g_last_tap_ms = 0;
int g_pending_taps = 0;
constexpr unsigned long kDoubleTapMs = 400UL;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void showWeather(bool force_fetch) {
  const bool should_fetch = WiFi.status() == WL_CONNECTED &&
                            (force_fetch || !services::weather::current().valid);
  if (should_fetch) {
    ui::weatherDisplayLoading();
    services::weather::fetch(services::location::lat(),
                             services::location::lon());
    g_last_weather_fetch_ms = millis();
  }
  ui::weatherDisplayDraw();
}

void showPage(Page page) {
  g_page = page;
  g_radar_visible = false;
  ui::weatherDisplayResetForecast();
  switch (page) {
    case Page::Weather:
      showWeather(true);
      break;
    case Page::Radar:
      showRadarIfConnected();
      break;
    case Page::Stats:
      ui::statsDisplayDraw();
      break;
  }
}

/** Double tap: toggle directly between Weather and Radar (never lands on
 *  Stats -- that's triple-tap-only). */
void toggleWeatherRadar() {
  showPage(g_page == Page::Radar ? Page::Weather : Page::Radar);
}

/** Triple tap: jump into the Stats debug page from wherever we are. */
void enterStats() {
  g_prev_page = g_page;
  g_page = Page::Stats;
  ui::statsDisplayDraw();
}

/** Any tap while on the Stats page leaves it immediately, back to whatever
 *  page was showing before Stats was entered. */
void leaveStats() {
  showPage(g_prev_page == Page::Stats ? Page::Weather : g_prev_page);
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootInputs() {
  bootButtonPollLongPress();

  if (bootButtonConsumeAutoCycleToggle()) {
    ui::weatherDisplayToggleAutoCycle();
    Serial.printf("Weather auto-cycle: %s\n",
                  ui::weatherDisplayAutoCycleEnabled() ? "ON" : "OFF");
    if (g_page == Page::Weather && WiFi.status() == WL_CONNECTED) {
      ui::weatherDisplayDraw();
    }
    return;
  }

  unsigned long tap_ms = 0;
  if (bootButtonConsumeTap(&tap_ms)) {
    if (g_page == Page::Stats) {
      // Every tap on the Stats page leaves it right away -- no need to
      // wait and see if more taps follow.
      g_pending_taps = 0;
      leaveStats();
      return;
    }

    if (g_pending_taps > 0 && tap_ms - g_last_tap_ms <= kDoubleTapMs) {
      ++g_pending_taps;
    } else {
      g_pending_taps = 1;
    }
    g_last_tap_ms = tap_ms;
    return;
  }

  if (g_pending_taps > 0 && millis() - g_last_tap_ms >= kDoubleTapMs) {
    const int taps = g_pending_taps;
    g_pending_taps = 0;
    if (taps >= 3) {
      enterStats();
    } else if (taps == 2) {
      toggleWeatherRadar();
    } else if (g_page == Page::Radar) {
      onRangeTap();
    } else if (g_page == Page::Weather) {
      ui::weatherDisplayAdvanceForecast();
      ui::weatherDisplayDraw();
    }
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootInputs();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  handleBootInputs();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showWeather(true);
  }
}

void loop() {
  handleBootInputs();
  wifiLoop();

  if (g_page == Page::Weather) {
    const unsigned long phase_4 = (millis() / 4000);
    const unsigned long phase_8 = (millis() / 8000);
    const bool phase_changed = phase_4 != g_last_weather_phase_4 ||
        phase_8 != g_last_weather_phase_8;
    if (WiFi.status() == WL_CONNECTED) {
      const services::weather::Data& w = services::weather::current();
      const unsigned long since = millis() - g_last_weather_fetch_ms;

      if (since >= config::kWeatherFetchIntervalMs ||
          (!w.valid && since >= config::kWeatherRetryIntervalMs)) {
        showWeather(true);
      } else if (ui::weatherDisplayAutoCycleTick()) {
        ui::weatherDisplayDraw();
        g_last_weather_phase_4 = phase_4;
        g_last_weather_phase_8 = phase_8;
      } else if (phase_changed) {
        ui::weatherDisplayDrawPartial();
        g_last_weather_phase_4 = phase_4;
        g_last_weather_phase_8 = phase_8;
      }
    } else if (phase_changed) {
      ui::weatherDisplayDraw();
      g_last_weather_phase_4 = phase_4;
      g_last_weather_phase_8 = phase_8;
    }
    delay(10);
    return;
  }

  if (g_page == Page::Stats) {
    const unsigned long phase_4 = (millis() / 4000);
    if (phase_4 != g_last_stats_phase_4) {
      g_last_stats_phase_4 = phase_4;
      ui::statsDisplayDraw();
    }
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect(false)) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    }
  }

  delay(10);
}
