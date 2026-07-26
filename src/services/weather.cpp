#include "services/weather.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

namespace services::weather {

namespace {

Data s_data;

// Cached US National Weather Service observation endpoint for the current
// location, so we only do the point->station lookup when the location changes.
String s_obs_url;
double s_station_lat = 1e9;
double s_station_lon = 1e9;
double s_city_lat = 1e9;
double s_city_lon = 1e9;

// NWS requires a descriptive User-Agent on every request.
constexpr char kUserAgent[] = "ESP32-PlaneRadar (github.com/mbronk/ESP32-Plane-Radar)";

// Hour offsets (from "now") used for the 4 forecast slots shown on the
// weather page: current, +2h, +6h, +24h.
constexpr int kForecastOffsets[4] = {0, 2, 6, 24};

void setLabel(char* out, const char* text) {
  constexpr size_t kLabelLength = sizeof(Data::label[0]);
  strncpy(out, text, kLabelLength - 1);
  out[kLabelLength - 1] = '\0';
}

void setConditionLabel(Condition cond, char* label) {
  switch (cond) {
    case Condition::Clear:
      setLabel(label, "Pogodnie");
      break;
    case Condition::PartlyCloudy:
      setLabel(label, "Cz\xc4\x99\xc5\x9b" "ciowe zachm.");
      break;
    case Condition::Cloudy:
      setLabel(label, "Zachmurzenie");
      break;
    case Condition::Fog:
      setLabel(label, "Mg\xc5\x82" "a");
      break;
    case Condition::Drizzle:
      setLabel(label, "M\xc5\xbc" "awka");
      break;
    case Condition::Rain:
      setLabel(label, "Deszcz");
      break;
    case Condition::FreezingRain:
      setLabel(label, "Marzn\xc4\x85" "cy deszcz");
      break;
    case Condition::Snow:
      setLabel(label, "\xc5\x9anieg");
      break;
    case Condition::Storm:
      setLabel(label, "Burza");
      break;
    case Condition::Hail:
      setLabel(label, "Burza gradowa");
      break;
    case Condition::Unknown:
    default:
      setLabel(label, "Nieznane");
      break;
  }
}

void setForecast(int idx, float temp_c, float feels_c, float temp_min_c,
                  float temp_max_c, float humidity, float precip_pct,
                  float pressure_hpa, float wind_speed_kmh,
                  float wind_gust_kmh, int wind_dir_deg,
                  float uv_index_max, const char* sunrise,
                  const char* sunset, int daylight_min, Condition cond,
                  Intensity intensity, const char* text,
                  const char* datetime) {
  if (idx < 0 || idx >= 4) {
    return;
  }
  s_data.temp_c[idx] = temp_c;
  s_data.feels_c[idx] = feels_c;
  s_data.temp_min_c[idx] = temp_min_c;
  s_data.temp_max_c[idx] = temp_max_c;
  s_data.humidity[idx] = humidity;
  s_data.precip_pct[idx] = precip_pct;
  s_data.pressure_hpa[idx] = pressure_hpa;
  s_data.wind_speed_kmh[idx] = wind_speed_kmh;
  s_data.wind_gust_kmh[idx] = wind_gust_kmh;
  s_data.wind_dir_deg[idx] = wind_dir_deg;
  s_data.uv_index_max[idx] = uv_index_max;
  if (sunrise != nullptr) {
    strncpy(s_data.sunrise[idx], sunrise, sizeof(s_data.sunrise[idx]) - 1);
    s_data.sunrise[idx][sizeof(s_data.sunrise[idx]) - 1] = '\0';
  } else {
    s_data.sunrise[idx][0] = '\0';
  }
  if (sunset != nullptr) {
    strncpy(s_data.sunset[idx], sunset, sizeof(s_data.sunset[idx]) - 1);
    s_data.sunset[idx][sizeof(s_data.sunset[idx]) - 1] = '\0';
  } else {
    s_data.sunset[idx][0] = '\0';
  }
  s_data.daylight_min[idx] = daylight_min;
  s_data.condition[idx] = cond;
  s_data.intensity[idx] = intensity;
  setLabel(s_data.label[idx], text);
  if (datetime != nullptr) {
    strncpy(s_data.datetime[idx], datetime, sizeof(s_data.datetime[idx]) - 1);
    s_data.datetime[idx][sizeof(s_data.datetime[idx]) - 1] = '\0';
  } else {
    s_data.datetime[idx][0] = '\0';
  }
}

int dayOfWeek(int y, int m, int d) {
  if (m < 3) {
    m += 12;
    y -= 1;
  }
  const int k = y % 100;
  const int j = y / 100;
  const int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 5) % 7;  // 0=Mon,1=Tue,...6=Sun
}

bool formatDateTime(const char* iso, char* out, size_t size) {
  if (iso == nullptr) {
    return false;
  }
  int year, month, day, hour, minute;
  if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d", &year, &month, &day, &hour,
             &minute) != 5) {
    return false;
  }

  static const char* kMonthNames[12] = {"sty", "lut", "mar", "kwi",
                                        "maj", "cze", "lip", "sie",
                                        "wrz", "paź", "lis", "gru"};
  static const char* kWeekdayLetters[7] = {"P", "W", "Ś", "C", "P", "S", "N"};

  const int wday = dayOfWeek(year, month, day);
  const char* day_letter = kWeekdayLetters[wday];
  const char* month_name = (month >= 1 && month <= 12) ?
      kMonthNames[month - 1] : "";

  std::snprintf(out, size, "%s %02d %s %02d:%02d", day_letter, day,
                month_name, hour, minute);
  return true;
}

bool formatTime(const char* iso, char* out, size_t size) {
  if (iso == nullptr) {
    return false;
  }
  int year, month, day, hour, minute;
  if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d", &year, &month, &day, &hour,
             &minute) != 5) {
    return false;
  }
  std::snprintf(out, size, "%02d:%02d", hour, minute);
  return true;
}

int minutesFromTime(const char* hhmm) {
  if (hhmm == nullptr) {
    return -1;
  }
  int hour = 0;
  int minute = 0;
  if (sscanf(hhmm, "%2d:%2d", &hour, &minute) != 2) {
    return -1;
  }
  return hour * 60 + minute;
}

class BlockingStream : public Stream {
 public:
  BlockingStream(Stream& inner, uint32_t timeout_ms)
      : inner_(inner), timeout_ms_(timeout_ms) {}
  int available() override { return inner_.available(); }
  int read() override { return wait() ? inner_.read() : -1; }
  int peek() override { return wait() ? inner_.peek() : -1; }
  size_t write(uint8_t) override { return 0; }

 private:
  bool wait() {
    const uint32_t start = millis();
    while (inner_.available() == 0) {
      if (millis() - start > timeout_ms_) {
        return false;
      }
      delay(1);
    }
    return true;
  }

  Stream& inner_;
  uint32_t timeout_ms_;
};

bool nwsOpen(WiFiClientSecure& client, HTTPClient& http, const String& url) {
  client.setInsecure();
  if (!http.begin(client, url)) {
    return false;
  }
  http.setUserAgent(kUserAgent);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Accept-Encoding", "identity");
  http.setTimeout(15000);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather(nws): HTTP %d\n", code);
    return false;
  }
  return true;
}

bool nwsGet(const String& url, JsonDocument& doc, const JsonDocument& filter) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!nwsOpen(client, http, url)) {
    http.end();
    return false;
  }

  BlockingStream blocking(http.getStream(), 12000);
  const DeserializationError err =
      deserializeJson(doc, blocking, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("weather(nws): JSON parse error: %s\n", err.c_str());
    return false;
  }
  return true;
}

void classifyNws(const char* desc, Condition* cond, Intensity* intensity,
                  char* label) {
  String d(desc ? desc : "");
  d.toLowerCase();
  const auto has = [&](const char* k) { return d.indexOf(k) >= 0; };

  if (has("light")) {
    *intensity = Intensity::Light;
  } else if (has("heavy") || has("violent") || has("dense")) {
    *intensity = Intensity::Heavy;
  } else {
    *intensity = Intensity::Moderate;
  }

  if (has("thunder") && has("hail")) {
    *cond = Condition::Hail;
  } else if (has("thunder")) {
    *cond = Condition::Storm;
  } else if (has("hail")) {
    *cond = Condition::Hail;
  } else if (has("freezing") && (has("rain") || has("drizzle"))) {
    *cond = Condition::FreezingRain;
  } else if (has("snow") || has("sleet") || has("ice") || has("flurr")) {
    *cond = Condition::Snow;
  } else if (has("drizzle")) {
    *cond = Condition::Drizzle;
  } else if (has("rain") || has("shower")) {
    *cond = Condition::Rain;
  } else if (has("fog") || has("haze") || has("mist") || has("smoke")) {
    *cond = Condition::Fog;
  } else if (has("partly") || has("few")) {
    *cond = Condition::PartlyCloudy;
  } else if (has("cloudy") || has("overcast")) {
    *cond = Condition::Cloudy;
  } else if (has("clear") || has("sunny") || has("fair")) {
    *cond = Condition::Clear;
  } else {
    *cond = Condition::Cloudy;
  }
  setConditionLabel(*cond, label);
}

bool nwsScanStationId(const String& url, char* out, size_t out_len) {
  JsonDocument filter;
  JsonObject f = filter["features"][0].to<JsonObject>();
  JsonObject props = f["properties"].to<JsonObject>();
  props["stationIdentifier"] = true;

  JsonDocument doc;
  if (!nwsGet(url, doc, filter)) {
    return false;
  }

  JsonArray features = doc["features"].as<JsonArray>();
  if (features.isNull() || features.size() == 0) {
    return false;
  }

  const char* station = features[0]["properties"]["stationIdentifier"].as<const char*>();
  if (station == nullptr || station[0] == '\0') {
    return false;
  }

  strncpy(out, station, out_len - 1);
  out[out_len - 1] = '\0';
  return true;
}

bool nwsResolveStation(double lat, double lon) {
  String purl = "https://api.weather.gov/points/" + String(lat, 4) + "," +
                String(lon, 4);
  JsonDocument pfilter;
  pfilter["properties"]["observationStations"] = true;
  JsonDocument pdoc;
  if (!nwsGet(purl, pdoc, pfilter)) {
    return false;
  }
  const char* stations_url =
      pdoc["properties"]["observationStations"].as<const char*>();
  if (stations_url == nullptr) {
    return false;
  }

  char sid[12];
  if (!nwsScanStationId(String(stations_url), sid, sizeof(sid))) {
    return false;
  }

  s_obs_url = String("https://api.weather.gov/stations/") + sid +
              "/observations/latest";
  s_station_lat = lat;
  s_station_lon = lon;
  Serial.printf("weather(nws): station %s\n", sid);
  return true;
}

bool fetchNws(double lat, double lon) {
  if (s_obs_url.length() == 0 || lat != s_station_lat ||
      lon != s_station_lon) {
    if (!nwsResolveStation(lat, lon)) {
      return false;
    }
  }

  JsonDocument filter;
  JsonObject fp = filter["properties"].to<JsonObject>();
  fp["temperature"]["value"] = true;
  fp["heatIndex"]["value"] = true;
  fp["windChill"]["value"] = true;
  fp["textDescription"] = true;

  JsonDocument doc;
  if (!nwsGet(s_obs_url, doc, filter)) {
    s_obs_url = "";
    return false;
  }

  JsonObject p = doc["properties"];
  if (p["temperature"]["value"].isNull()) {
    return false;
  }
  const float temp_c = p["temperature"]["value"].as<float>();

  float feels_c = temp_c;
  if (!p["heatIndex"]["value"].isNull()) {
    feels_c = p["heatIndex"]["value"].as<float>();
  } else if (!p["windChill"]["value"].isNull()) {
    feels_c = p["windChill"]["value"].as<float>();
  }

  Condition cond;
  Intensity intensity;
  char label[24];
  classifyNws(p["textDescription"].as<const char*>(), &cond, &intensity,
              label);

  char datetime[24] = {0};
  const char* current_time = doc["properties"]["timestamp"].as<const char*>();
  if (!formatDateTime(current_time, datetime, sizeof(datetime))) {
    datetime[0] = '\0';
  }

  for (int i = 0; i < 4; ++i) {
    setForecast(i, temp_c, feels_c, -1.0f, -1.0f, -1.0f, -1.0f,
                -1.0f, -1.0f, -1.0f, -1, -1.0f, nullptr, nullptr, -1,
                cond, intensity, label, datetime);
  }
  s_data.valid = true;
  Serial.printf("weather(nws): %.0fC feels %.0fC (%s)\n", temp_c, feels_c,
                label);
  return true;
}

// Open-Meteo's WMO weather codes encode intensity directly (each condition
// family is usually 3 codes: light/slight, moderate, heavy/dense/violent,
// with a couple of 2-code families that only distinguish light vs. heavy).
// See https://open-meteo.com/en/docs for the full WW code table.
void classifyOpenMeteo(int code, Condition* cond, Intensity* intensity,
                        char* label) {
  *intensity = Intensity::Moderate;
  if (code == 0) {
    *cond = Condition::Clear;
  } else if (code == 1 || code == 2) {
    *cond = Condition::PartlyCloudy;
  } else if (code == 3) {
    *cond = Condition::Cloudy;
  } else if (code == 45 || code == 48) {
    *cond = Condition::Fog;
  } else if (code >= 51 && code <= 55) {
    // Drizzle: 51 light, 53 moderate, 55 dense.
    *cond = Condition::Drizzle;
    *intensity = code == 51 ? Intensity::Light
                            : (code == 55 ? Intensity::Heavy
                                          : Intensity::Moderate);
  } else if (code == 56 || code == 57) {
    // Freezing drizzle: 56 light, 57 dense.
    *cond = Condition::FreezingRain;
    *intensity = code == 56 ? Intensity::Light : Intensity::Heavy;
  } else if (code >= 61 && code <= 65) {
    // Rain: 61 slight, 63 moderate, 65 heavy.
    *cond = Condition::Rain;
    *intensity = code == 61 ? Intensity::Light
                            : (code == 65 ? Intensity::Heavy
                                          : Intensity::Moderate);
  } else if (code == 66 || code == 67) {
    // Freezing rain: 66 light, 67 heavy.
    *cond = Condition::FreezingRain;
    *intensity = code == 66 ? Intensity::Light : Intensity::Heavy;
  } else if (code >= 71 && code <= 75) {
    // Snow fall: 71 slight, 73 moderate, 75 heavy.
    *cond = Condition::Snow;
    *intensity = code == 71 ? Intensity::Light
                            : (code == 75 ? Intensity::Heavy
                                          : Intensity::Moderate);
  } else if (code == 77) {
    // Snow grains: fine, sparse flakes -- treat as light snow.
    *cond = Condition::Snow;
    *intensity = Intensity::Light;
  } else if (code >= 80 && code <= 82) {
    // Rain showers: 80 slight, 81 moderate, 82 violent.
    *cond = Condition::Rain;
    *intensity = code == 80 ? Intensity::Light
                            : (code == 82 ? Intensity::Heavy
                                          : Intensity::Moderate);
  } else if (code == 85 || code == 86) {
    // Snow showers: 85 slight, 86 heavy.
    *cond = Condition::Snow;
    *intensity = code == 85 ? Intensity::Light : Intensity::Heavy;
  } else if (code == 95) {
    // Thunderstorm: slight or moderate (no separate WMO code split).
    *cond = Condition::Storm;
  } else if (code == 96 || code == 99) {
    // Thunderstorm with hail: 96 slight, 99 heavy.
    *cond = Condition::Hail;
    *intensity = code == 96 ? Intensity::Light : Intensity::Heavy;
  } else {
    *cond = Condition::Unknown;
  }
  setConditionLabel(*cond, label);
}

bool resolveCityName(double lat, double lon, char* out, size_t out_len) {
  if (std::abs(lat - s_city_lat) < 1e-6 &&
      std::abs(lon - s_city_lon) < 1e-6 && out[0] != '\0') {
    return true;
  }

  String url = "https://nominatim.openstreetmap.org/reverse?format=json&lat=";
  url += String(lat, 6);
  url += "&lon=";
  url += String(lon, 6);
  url += "&zoom=10&addressdetails=1";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("weather: geocode begin failed");
    return false;
  }

  http.setUserAgent(kUserAgent);
  http.setTimeout(10000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather: geocode HTTP %d\n", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("weather: geocode JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonObject address = doc["address"].as<JsonObject>();
  const char* name = nullptr;
  if (!address["city"].isNull()) {
    name = address["city"].as<const char*>();
  } else if (!address["town"].isNull()) {
    name = address["town"].as<const char*>();
  } else if (!address["village"].isNull()) {
    name = address["village"].as<const char*>();
  } else if (!address["hamlet"].isNull()) {
    name = address["hamlet"].as<const char*>();
  } else if (!address["municipality"].isNull()) {
    name = address["municipality"].as<const char*>();
  } else if (!address["county"].isNull()) {
    name = address["county"].as<const char*>();
  } else if (!address["state"].isNull()) {
    name = address["state"].as<const char*>();
  }

  if (name == nullptr || name[0] == '\0') {
    const char* display_name = doc["display_name"].as<const char*>();
    name = display_name;
  }

  if (name == nullptr || name[0] == '\0') {
    return false;
  }

  strncpy(out, name, out_len - 1);
  out[out_len - 1] = '\0';
  s_city_lat = lat;
  s_city_lon = lon;
  return true;
}

bool fetchOpenMeteo(double lat, double lon) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(lat, 5);
  url += "&longitude=";
  url += String(lon, 5);
  url +=
      "&current_weather=true"
      "&hourly=temperature_2m,apparent_temperature,weathercode,relativehumidity_2m,precipitation_probability,pressure_msl,winddirection_10m,windspeed_10m,windgusts_10m"
      "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
      "&temperature_unit=celsius"
      "&timezone=auto"
      // Default forecast_days=7 pulls ~11KB of JSON (168 hourly rows), which
      // eats enough heap on the ESP32-C3 that the JsonDocument can silently
      // overflow while parsing the huge 'hourly' block -- and since 'daily'
      // comes *after* 'hourly' in the response, its fields (temp min/max,
      // UV) end up null and read back as 0. We only ever look 24h ahead, so
      // 3 days of hourly/daily data is comfortably enough margin.
      "&forecast_days=3";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("weather: http.begin failed");
    return false;
  }

  http.setTimeout(10000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather: HTTP %d url=%s\n", code, url.c_str());
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("weather: JSON parse error: %s\n", err.c_str());
    return false;
  }
  if (doc.overflowed()) {
    // The document ran out of memory mid-parse; some fields (typically
    // 'daily', since it comes last in the response) were silently dropped
    // and would read back as null/0 instead of real values. Bail out and
    // let the caller retry/fall back rather than display bogus zeros.
    Serial.println("weather: JSON document overflowed, discarding partial parse");
    return false;
  }

  JsonObject cur = doc["current_weather"].as<JsonObject>();
  if (cur.isNull()) {
    Serial.println("weather: no 'current_weather' object");
    return false;
  }

  const char* current_time = cur["time"].as<const char*>();
  if (current_time == nullptr) {
    return false;
  }

  const float current_temp = cur["temperature"].as<float>();
  const int current_code = cur["weathercode"].as<int>();

  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray temps = doc["hourly"]["temperature_2m"].as<JsonArray>();
  JsonArray feels = doc["hourly"]["apparent_temperature"].as<JsonArray>();
  JsonArray humid = doc["hourly"]["relativehumidity_2m"].as<JsonArray>();
  JsonArray precip = doc["hourly"]["precipitation_probability"].as<JsonArray>();
  JsonArray pressure = doc["hourly"]["pressure_msl"].as<JsonArray>();
  JsonArray codes = doc["hourly"]["weathercode"].as<JsonArray>();
  JsonArray wind_dirs = doc["hourly"]["winddirection_10m"].as<JsonArray>();
  JsonArray wind_speeds = doc["hourly"]["windspeed_10m"].as<JsonArray>();
  JsonArray wind_gusts = doc["hourly"]["windgusts_10m"].as<JsonArray>();
  JsonObject daily = doc["daily"].as<JsonObject>();
  JsonArray daily_times = daily["time"].as<JsonArray>();
  JsonArray daily_temp_min = daily["temperature_2m_min"].as<JsonArray>();
  JsonArray daily_temp_max = daily["temperature_2m_max"].as<JsonArray>();
  JsonArray daily_uv = daily["uv_index_max"].as<JsonArray>();
  JsonArray daily_sunrise = daily["sunrise"].as<JsonArray>();
  JsonArray daily_sunset = daily["sunset"].as<JsonArray>();
  if (times.size() == 0 || temps.size() == 0 || feels.size() == 0 ||
      humid.size() == 0 || precip.size() == 0 || pressure.size() == 0 ||
      codes.size() == 0 || wind_dirs.size() == 0 || wind_speeds.size() == 0 ||
      wind_gusts.size() == 0 || daily_times.size() == 0 ||
      daily_temp_min.size() == 0 || daily_temp_max.size() == 0 ||
      daily_uv.size() == 0 || daily_sunrise.size() == 0 ||
      daily_sunset.size() == 0) {
    return false;
  }

  size_t base_idx = 0;
  bool found_current_hour = false;
  for (size_t i = 0; i < times.size(); ++i) {
    const char* t = times[i].as<const char*>();
    if (t != nullptr && strcmp(t, current_time) == 0) {
      base_idx = i;
      found_current_hour = true;
      break;
    }
  }

  if (!found_current_hour) {
    for (size_t i = 0; i < times.size(); ++i) {
      const char* t = times[i].as<const char*>();
      if (t != nullptr && strncmp(t, current_time, 13) == 0) {
        base_idx = i;
        found_current_hour = true;
        break;
      }
    }
  }

  float current_feels = current_temp;
  float current_humidity = -1.0f;
  float current_precip = -1.0f;
  float current_pressure = -1.0f;
  float current_wind_speed = -1.0f;
  float current_wind_gust = -1.0f;
  int current_wind_dir = -1;
  if (found_current_hour) {
    current_feels = feels[base_idx].as<float>();
    current_humidity = humid[base_idx].as<float>();
    current_precip = precip[base_idx].as<float>();
    current_pressure = pressure[base_idx].as<float>();
    current_wind_speed = wind_speeds[base_idx].as<float>();
    current_wind_gust = wind_gusts[base_idx].as<float>();
    current_wind_dir = wind_dirs[base_idx].as<int>();
  } else {
    if (feels.size() > 0) {
      current_feels = feels[0].as<float>();
    }
    if (humid.size() > 0) {
      current_humidity = humid[0].as<float>();
    }
    if (precip.size() > 0) {
      current_precip = precip[0].as<float>();
    }
    if (pressure.size() > 0) {
      current_pressure = pressure[0].as<float>();
    }
    if (wind_speeds.size() > 0) {
      current_wind_speed = wind_speeds[0].as<float>();
    }
    if (wind_gusts.size() > 0) {
      current_wind_gust = wind_gusts[0].as<float>();
    }
    if (wind_dirs.size() > 0) {
      current_wind_dir = wind_dirs[0].as<int>();
    }
  }

  Condition current_cond;
  Intensity current_intensity;
  char current_label[24];
  classifyOpenMeteo(current_code, &current_cond, &current_intensity,
                     current_label);

  auto findDailyIndex = [&](const char* time_iso) {
    // -1 means "no matching day" -- callers must treat that as missing data
    // (n/a) rather than silently falling back to day 0's values.
    if (time_iso == nullptr) {
      return -1;
    }
    char date[11] = {0};
    strncpy(date, time_iso, 10);
    for (size_t i = 0; i < daily_times.size(); ++i) {
      const char* daily_time = daily_times[i].as<const char*>();
      if (daily_time != nullptr && strncmp(daily_time, date, 10) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  auto dailyDaylightMin = [&](const char* sunrise_iso, const char* sunset_iso) {
    char sunrise_time[8] = {0};
    char sunset_time[8] = {0};
    if (!formatTime(sunrise_iso, sunrise_time, sizeof(sunrise_time)) ||
        !formatTime(sunset_iso, sunset_time, sizeof(sunset_time))) {
      return -1;
    }
    const int sr = minutesFromTime(sunrise_time);
    const int ss = minutesFromTime(sunset_time);
    if (sr < 0 || ss < 0 || ss < sr) {
      return -1;
    }
    return ss - sr;
  };

  char current_datetime[24] = {0};
  if (!formatDateTime(current_time, current_datetime, sizeof(current_datetime))) {
    current_datetime[0] = '\0';
  }
  const int current_daily_idx = findDailyIndex(current_time);
  char current_sunrise[8] = {0};
  char current_sunset[8] = {0};
  float current_uv = -1.0f;
  float current_temp_min = -1.0f;
  float current_temp_max = -1.0f;
  int current_daylight = -1;
  if (current_daily_idx >= 0 &&
      static_cast<size_t>(current_daily_idx) < daily_times.size()) {
    formatTime(daily_sunrise[current_daily_idx].as<const char*>(),
               current_sunrise, sizeof(current_sunrise));
    formatTime(daily_sunset[current_daily_idx].as<const char*>(),
               current_sunset, sizeof(current_sunset));
    current_daylight = dailyDaylightMin(
        daily_sunrise[current_daily_idx].as<const char*>(),
        daily_sunset[current_daily_idx].as<const char*>());
    current_uv = daily_uv[current_daily_idx].as<float>();
    current_temp_min = daily_temp_min[current_daily_idx].as<float>();
    current_temp_max = daily_temp_max[current_daily_idx].as<float>();
  }
  setForecast(0, current_temp, current_feels, current_temp_min,
              current_temp_max, current_humidity, current_precip,
              current_pressure, current_wind_speed, current_wind_gust,
              current_wind_dir, current_uv, current_sunrise, current_sunset,
              current_daylight, current_cond, current_intensity,
              current_label, current_datetime);

  for (int idx = 1; idx < 4; ++idx) {
    size_t pos = base_idx + kForecastOffsets[idx];
    if (pos >= times.size()) {
      pos = times.size() - 1;
    }
    const float temp_c = temps[pos].as<float>();
    const float feels_c = feels[pos].as<float>();
    const float humidity_pct = humid[pos].as<float>();
    const float precip_pct = precip[pos].as<float>();
    const float pressure_hpa = pressure[pos].as<float>();
    const float wind_speed = wind_speeds[pos].as<float>();
    const float wind_gust = wind_gusts[pos].as<float>();
    const int wind_dir = wind_dirs[pos].as<int>();
    const int code = codes[pos].as<int>();
    const char* time_str = times[pos].as<const char*>();
    char datetime_str[24] = {0};
    if (!formatDateTime(time_str, datetime_str, sizeof(datetime_str))) {
      datetime_str[0] = '\0';
    }
    const int day_idx = findDailyIndex(time_str);
    float temp_min_c = -1.0f;
    float temp_max_c = -1.0f;
    float uv_max = -1.0f;
    char sunrise_text[8] = {0};
    char sunset_text[8] = {0};
    int daylight_min = -1;
    if (day_idx >= 0 && static_cast<size_t>(day_idx) < daily_times.size()) {
      temp_min_c = daily_temp_min[day_idx].as<float>();
      temp_max_c = daily_temp_max[day_idx].as<float>();
      uv_max = daily_uv[day_idx].as<float>();
      formatTime(daily_sunrise[day_idx].as<const char*>(), sunrise_text,
                 sizeof(sunrise_text));
      formatTime(daily_sunset[day_idx].as<const char*>(), sunset_text,
                 sizeof(sunset_text));
      daylight_min = dailyDaylightMin(
          daily_sunrise[day_idx].as<const char*>(),
          daily_sunset[day_idx].as<const char*>());
    }
    Condition cond;
    Intensity intensity;
    char label[24];
    classifyOpenMeteo(code, &cond, &intensity, label);
    setForecast(idx, temp_c, feels_c, temp_min_c, temp_max_c,
                humidity_pct, precip_pct, pressure_hpa, wind_speed,
                wind_gust, wind_dir, uv_max, sunrise_text, sunset_text,
                daylight_min, cond, intensity, label, datetime_str);
  }

  if (!resolveCityName(lat, lon, s_data.location,
                       sizeof(s_data.location))) {
    s_data.location[0] = '\0';
  }
  s_data.elevation_m =
      doc["elevation"].isNull() ? -1.0f : doc["elevation"].as<float>();

  s_data.valid = true;
  Serial.printf("weather(open-meteo): now %.0fC feels %.0fC (%s) @ %s\n",
                s_data.temp_c[0], s_data.feels_c[0], s_data.label[0],
                s_data.location[0] != '\0' ? s_data.location : "(no name)");
  Serial.printf(
      "weather(open-meteo): daily_idx=%d min/max=%.1f/%.1fC uv=%.1f "
      "slots minmax/uv: [0]%.1f/%.1f/%.1f [1]%.1f/%.1f/%.1f "
      "[2]%.1f/%.1f/%.1f [3]%.1f/%.1f/%.1f\n",
      current_daily_idx, current_temp_min, current_temp_max, current_uv,
      s_data.temp_min_c[0], s_data.temp_max_c[0], s_data.uv_index_max[0],
      s_data.temp_min_c[1], s_data.temp_max_c[1], s_data.uv_index_max[1],
      s_data.temp_min_c[2], s_data.temp_max_c[2], s_data.uv_index_max[2],
      s_data.temp_min_c[3], s_data.temp_max_c[3], s_data.uv_index_max[3]);
  return true;
}

// Best-effort European Air Quality Index fetch from Open-Meteo's separate
// Air Quality API. This is entirely optional: on any failure it just leaves
// aqi_european[] at the -1 sentinel (meaning "don't render") without
// affecting the main weather fetch's success/failure.
void fetchAirQuality(double lat, double lon) {
  for (int i = 0; i < 4; ++i) {
    s_data.aqi_european[i] = -1.0f;
  }

  String url = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=";
  url += String(lat, 5);
  url += "&longitude=";
  url += String(lon, 5);
  url += "&current=european_aqi&hourly=european_aqi&forecast_days=2&timezone=auto";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("weather(aqi): http.begin failed");
    return;
  }

  http.setTimeout(10000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather(aqi): HTTP %d\n", code);
    http.end();
    return;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("weather(aqi): JSON parse error: %s\n", err.c_str());
    return;
  }
  if (doc.overflowed()) {
    Serial.println("weather(aqi): JSON document overflowed, discarding");
    return;
  }

  const char* current_time = doc["current"]["time"].as<const char*>();
  if (current_time == nullptr || doc["current"]["european_aqi"].isNull()) {
    Serial.println("weather(aqi): no 'current' european_aqi value");
    return;
  }
  s_data.aqi_european[0] = doc["current"]["european_aqi"].as<float>();

  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray aqis = doc["hourly"]["european_aqi"].as<JsonArray>();
  if (times.size() == 0 || aqis.size() == 0) {
    return;
  }

  size_t base_idx = 0;
  bool found = false;
  for (size_t i = 0; i < times.size(); ++i) {
    const char* t = times[i].as<const char*>();
    if (t != nullptr && strcmp(t, current_time) == 0) {
      base_idx = i;
      found = true;
      break;
    }
  }
  if (!found) {
    for (size_t i = 0; i < times.size(); ++i) {
      const char* t = times[i].as<const char*>();
      if (t != nullptr && strncmp(t, current_time, 13) == 0) {
        base_idx = i;
        found = true;
        break;
      }
    }
  }
  if (!found) {
    return;
  }

  for (int idx = 1; idx < 4; ++idx) {
    size_t pos = base_idx + kForecastOffsets[idx];
    if (pos >= aqis.size()) {
      pos = aqis.size() - 1;
    }
    s_data.aqi_european[idx] = aqis[pos].as<float>();
  }

  Serial.printf("weather(aqi): slots [0]%.0f [1]%.0f [2]%.0f [3]%.0f\n",
                s_data.aqi_european[0], s_data.aqi_european[1],
                s_data.aqi_european[2], s_data.aqi_european[3]);
}

}  // namespace

const Data& current() { return s_data; }

bool fetch(double lat, double lon) {
  bool ok = fetchOpenMeteo(lat, lon);
  if (!ok) {
    Serial.println("weather: Open-Meteo unavailable, falling back to NWS");
    ok = fetchNws(lat, lon);
  }
  if (ok) {
    fetchAirQuality(lat, lon);
  }
  return ok;
}

}  // namespace services::weather
