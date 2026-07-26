#pragma once

#include <cstddef>

namespace services::weather {

/** Coarse condition class for picking an icon/color on the weather page. */
enum class Condition {
  Unknown,
  Clear,
  PartlyCloudy,
  Cloudy,
  Fog,
  Drizzle,
  Rain,
  FreezingRain,
  Snow,
  Storm,
  Hail,
};

/**
 * Precipitation/severity intensity within a Condition, e.g. light drizzle
 * vs. a heavy rain shower. Used to vary the icon (and could be used for
 * label text) without needing a separate Condition value per WMO code.
 */
enum class Intensity {
  Light,
  Moderate,
  Heavy,
};

struct Data {
  bool valid = false;
  float temp_c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float feels_c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float temp_min_c[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float temp_max_c[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float humidity[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float precip_pct[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float pressure_hpa[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float wind_speed_kmh[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  float wind_gust_kmh[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  int wind_dir_deg[4] = {-1, -1, -1, -1};
  float uv_index_max[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  // European Air Quality Index (0-20 good ... 100+ extremely poor). Fetched
  // via a separate, best-effort Open-Meteo Air Quality API call -- stays at
  // the -1 sentinel (not rendered) if that call fails or isn't reached.
  float aqi_european[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  char sunrise[4][8] = {{0}};  // HH:MM
  char sunset[4][8] = {{0}};   // HH:MM
  int daylight_min[4] = {-1, -1, -1, -1};
  Condition condition[4] = {Condition::Unknown, Condition::Unknown,
                            Condition::Unknown, Condition::Unknown};
  Intensity intensity[4] = {Intensity::Moderate, Intensity::Moderate,
                            Intensity::Moderate, Intensity::Moderate};
  char label[4][24] = {{0}};  // e.g. "Pogodne", "Deszcz"
  char datetime[4][24] = {{0}};  // e.g. "25.07 10:45"
  char location[32] = {0};
  float elevation_m = -1.0f;
};

/** Last fetched weather (valid == false until the first successful fetch). */
const Data& current();

/**
 * Fetch current weather from Open-Meteo for the given coordinates.
 * Returns true and updates current() on success. Requires Wi-Fi.
 */
bool fetch(double lat, double lon);

}  // namespace services::weather
