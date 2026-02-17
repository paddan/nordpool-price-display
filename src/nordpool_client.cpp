#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "logging_utils.h"
#include "nordpool_client.h"
#include "time_utils.h"

namespace {
constexpr uint32_t kHttpTimeoutMs = 10000;
constexpr uint16_t kMovingAverageWindowHours = 72;
constexpr uint16_t kMaxMovingAverageWindowSamples = kMovingAverageWindowHours * 4;  // 15-minute resolution
constexpr char kMovingAveragePath[] = "/nordpool_ma.bin";
constexpr float kDefaultMovingAverageKrPerKwh = 1.0f;
constexpr uint32_t kAvgStoreMagic = 0x4E504D41;  // "NPMA"
constexpr uint16_t kAvgStoreVersion = 2;

struct MovingAverageStore {
  uint32_t magic = kAvgStoreMagic;
  uint16_t version = kAvgStoreVersion;
  uint16_t resolutionMinutes = 60;
  uint16_t windowSamples = kMovingAverageWindowHours;
  uint16_t count = 0;
  uint16_t head = 0;  // next write index
  char lastSlotKey[20] = {0};  // YYYY-MM-DDTHH or YYYY-MM-DDTHH:MM
  float values[kMaxMovingAverageWindowSamples] = {0.0f};
};

float applyCustomPriceFormula(float rawPriceKrPerKwh) {
  // Apply formula in ore: 1.25 * hourly_price + 84.225, then convert back to kr.
  const float rawOre = rawPriceKrPerKwh * 100.0f;
  const float adjustedOre = (1.25f * rawOre) + 84.225f;
  return adjustedOre / 100.0f;
}

bool formatDate(time_t ts, char *out, size_t outSize) {
  struct tm localTm;
  if (!localtime_r(&ts, &localTm)) return false;
  return strftime(out, outSize, "%Y-%m-%d", &localTm) > 0;
}

bool parseUtcIso(const String &iso, struct tm &tmUtc) {
  if (iso.length() < 19) return false;

  const char *s = iso.c_str();
  auto parse2 = [](const char *p, int &out) -> bool {
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])) return false;
    out = ((p[0] - '0') * 10) + (p[1] - '0');
    return true;
  };
  auto parse4 = [](const char *p, int &out) -> bool {
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
        !isdigit((unsigned char)p[2]) || !isdigit((unsigned char)p[3])) {
      return false;
    }
    out = ((p[0] - '0') * 1000) + ((p[1] - '0') * 100) + ((p[2] - '0') * 10) + (p[3] - '0');
    return true;
  };

  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse4(&s[0], year) || !parse2(&s[5], month) || !parse2(&s[8], day) ||
      !parse2(&s[11], hour) || !parse2(&s[14], minute) || !parse2(&s[17], second)) {
    return false;
  }

  tmUtc = {};
  tmUtc.tm_year = year - 1900;
  tmUtc.tm_mon = month - 1;
  tmUtc.tm_mday = day;
  tmUtc.tm_hour = hour;
  tmUtc.tm_min = minute;
  tmUtc.tm_sec = second;

  return true;
}

uint16_t movingAverageWindowForResolution(uint16_t resolutionMinutes) {
  const uint16_t normalizedResolution = normalizeResolutionMinutes(resolutionMinutes);
  return (uint16_t)((kMovingAverageWindowHours * 60) / normalizedResolution);
}

bool isIntervalKey(const String &value) {
  return value.length() == 13 || value.length() == 16;
}

bool ensureSpiffsMounted() {
  static bool attempted = false;
  static bool mounted = false;
  if (!attempted) {
    attempted = true;
    mounted = SPIFFS.begin(true);
    logf("SPIFFS mount: %s", mounted ? "ok" : "failed");
    if (mounted) {
      logf("SPIFFS info: used=%u total=%u", (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    }
  }
  return mounted;
}

void resetStore(MovingAverageStore &store) {
  store = MovingAverageStore();
}

bool loadStore(MovingAverageStore &store) {
  if (!ensureSpiffsMounted()) return false;

  File file = SPIFFS.open(kMovingAveragePath, FILE_READ);
  if (!file) return false;

  if ((size_t)file.size() != sizeof(MovingAverageStore)) {
    file.close();
    return false;
  }

  const size_t readBytes = file.read((uint8_t *)&store, sizeof(MovingAverageStore));
  file.close();
  if (readBytes != sizeof(MovingAverageStore)) return false;
  if (store.magic != kAvgStoreMagic || store.version != kAvgStoreVersion) return false;
  store.resolutionMinutes = normalizeResolutionMinutes(store.resolutionMinutes);
  const uint16_t expectedWindow = movingAverageWindowForResolution(store.resolutionMinutes);
  if (store.windowSamples != expectedWindow) return false;
  if (store.windowSamples == 0 || store.windowSamples > kMaxMovingAverageWindowSamples) return false;
  if (store.head >= store.windowSamples) return false;
  if (store.count > store.windowSamples) return false;
  return true;
}

bool saveStore(const MovingAverageStore &store) {
  if (!ensureSpiffsMounted()) return false;

  File file = SPIFFS.open(kMovingAveragePath, FILE_WRITE);
  if (!file) return false;

  const size_t written = file.write((const uint8_t *)&store, sizeof(MovingAverageStore));
  file.flush();
  file.close();
  return written == sizeof(MovingAverageStore);
}

void addMovingAverageSample(MovingAverageStore &store, float value) {
  if (store.windowSamples == 0 || store.windowSamples > kMaxMovingAverageWindowSamples) {
    store.windowSamples = kMovingAverageWindowHours;
  }

  store.values[store.head] = value;
  store.head = (store.head + 1) % store.windowSamples;
  if (store.count < store.windowSamples) ++store.count;
}

float movingAverageValue(const MovingAverageStore &store) {
  if (store.count == 0) return 0.0f;

  float sum = 0.0f;
  for (size_t i = 0; i < store.count; ++i) {
    sum += store.values[i];
  }
  return sum / (float)store.count;
}

String classifyLevelFromAverage(float priceKrPerKwh, float movingAvgKrPerKwh) {
  if (movingAvgKrPerKwh <= 0.0001f) return "UNKNOWN";

  const float ratio = priceKrPerKwh / movingAvgKrPerKwh;
  if (ratio <= 0.60f) return "VERY_CHEAP";
  if (ratio <= 0.90f) return "CHEAP";
  if (ratio < 1.15f) return "NORMAL";
  if (ratio < 1.40f) return "EXPENSIVE";
  return "VERY_EXPENSIVE";
}

void applyLevelsFromMovingAverage(PriceState &state, float movingAvgKrPerKwh) {
  for (size_t i = 0; i < state.count; ++i) {
    state.points[i].level = classifyLevelFromAverage(state.points[i].price, movingAvgKrPerKwh);
  }
}

bool updateHistoryFromPoints(PriceState &state, MovingAverageStore &store) {
  bool changed = false;
  String lastPersisted = String(store.lastSlotKey);
  for (size_t i = 0; i < state.count; ++i) {
    const String pointKey = intervalKeyFromIso(state.points[i].startsAt, state.resolutionMinutes);
    if (!isIntervalKey(pointKey)) continue;
    if (isIntervalKey(lastPersisted) && pointKey <= lastPersisted) continue;  // already processed

    // Include all available fetched points (today + tomorrow) in the rolling history.
    addMovingAverageSample(store, state.points[i].price);
    strncpy(store.lastSlotKey, pointKey.c_str(), sizeof(store.lastSlotKey) - 1);
    store.lastSlotKey[sizeof(store.lastSlotKey) - 1] = '\0';
    lastPersisted = pointKey;
    changed = true;
  }
  return changed;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= (month <= 2);
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);                                  // [0, 399]
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;     // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                         // [0, 146096]
  return era * 146097 + (int64_t)doe - 719468;
}

time_t utcToEpochSeconds(const struct tm &tmUtc) {
  const int year = tmUtc.tm_year + 1900;
  const unsigned month = (unsigned)tmUtc.tm_mon + 1;
  const unsigned day = (unsigned)tmUtc.tm_mday;
  const int64_t days = daysFromCivil(year, month, day);
  const int64_t sec = days * 86400 + (int64_t)tmUtc.tm_hour * 3600 + (int64_t)tmUtc.tm_min * 60 + tmUtc.tm_sec;
  if (sec < 0) return 0;
  return (time_t)sec;
}

String utcIsoToLocalIsoSlot(const String &utcIso) {
  struct tm tmUtc;
  if (!parseUtcIso(utcIso, tmUtc)) return utcIso;

  const time_t epochUtc = utcToEpochSeconds(tmUtc);
  if (epochUtc <= 0) return utcIso;

  struct tm localTm;
  if (!localtime_r(&epochUtc, &localTm)) return utcIso;

  char out[32];
  strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:00", &localTm);
  return String(out);
}

bool addPoints(JsonArray arr, const char *area, PriceState &state) {
  if (arr.isNull()) return false;

  bool added = false;
  for (JsonObject item : arr) {
    if (state.count >= kMaxPoints) return added;

    JsonObject entryPerArea = item["entryPerArea"];
    if (entryPerArea.isNull()) continue;

    const JsonVariant selected = entryPerArea[area];
    if (selected.isNull()) continue;

    // Nord Pool index prices are in currency/MWh. Convert to kr/kWh.
    const float nordPoolPricePerMwh = selected | 0.0f;
    const float energyPriceKrPerKwh = nordPoolPricePerMwh / 1000.0f;
    const float adjustedPrice = applyCustomPriceFormula(energyPriceKrPerKwh);

    PricePoint &p = state.points[state.count++];
    p.startsAt = utcIsoToLocalIsoSlot(String((const char *)(item["deliveryStart"] | "")));
    p.price = adjustedPrice;
    p.level = "UNKNOWN";
    added = true;
  }

  return added;
}

bool fetchDate(
    HTTPClient &http,
    WiFiClientSecure &client,
    const char *apiBaseUrl,
    const char *date,
    const char *area,
    const char *currency,
    uint16_t resolutionMinutes,
    PriceState &out
) {
  const uint16_t normalizedResolution = normalizeResolutionMinutes(resolutionMinutes);
  char url[256];
  snprintf(
      url,
      sizeof(url),
      "%s?date=%s&market=DayAhead&indexNames=%s&currency=%s&resolutionInMinutes=%u",
      apiBaseUrl,
      date,
      area,
      currency,
      (unsigned)normalizedResolution);

  http.useHTTP10(true);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    out.error = "HTTP begin failed";
    return false;
  }
  http.addHeader("Accept-Encoding", "identity");

  const int status = http.GET();
  logf("Nord Pool GET %s status=%d", date, status);
  if (status == 204) {
    http.end();
    return true;
  }
  if (status != 200) {
    out.error = status <= 0 ? "HTTP GET failed" : ("HTTP " + String(status));
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["title"] = true;
  filter["currency"] = true;
  JsonArray entriesFilter = filter["multiIndexEntries"].to<JsonArray>();
  JsonObject entryFilter = entriesFilter.add<JsonObject>();
  entryFilter["deliveryStart"] = true;
  entryFilter["entryPerArea"][area] = true;
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    out.error = (err == DeserializationError::EmptyInput) ? "Empty response body" : "JSON parse failed";
    logf("Nord Pool JSON parse error: %s", err.c_str());
    return false;
  }

  if (!doc["title"].isNull() && String((const char *)(doc["title"] | "")) == "Unauthorized") {
    out.error = "Nord Pool API unauthorized";
    return false;
  }

  if (!doc["currency"].isNull()) {
    out.currency = String((const char *)(doc["currency"] | currency));
  }

  addPoints(doc["multiIndexEntries"], area, out);
  return true;
}

void assignCurrentFromClock(PriceState &out) {
  out.currentIndex = findCurrentPricePointIndex(out, out.resolutionMinutes);
  if (out.currentIndex < 0) return;

  const PricePoint &point = out.points[out.currentIndex];
  out.currentStartsAt = point.startsAt;
  out.currentPrice = point.price;
}

void assignCurrentLevel(PriceState &out) {
  if (out.currentIndex < 0 || out.currentIndex >= (int)out.count) return;
  out.currentLevel = out.points[out.currentIndex].level;
}

uint16_t applyMovingAverageToState(PriceState &state) {
  if (state.count == 0) return 0;

  state.resolutionMinutes = normalizeResolutionMinutes(state.resolutionMinutes);
  const uint16_t targetWindow = movingAverageWindowForResolution(state.resolutionMinutes);

  MovingAverageStore store;
  if (!loadStore(store)) {
    resetStore(store);
  }
  if (store.resolutionMinutes != state.resolutionMinutes || store.windowSamples != targetWindow) {
    resetStore(store);
    store.resolutionMinutes = state.resolutionMinutes;
    store.windowSamples = targetWindow;
  }

  const bool historyChanged = updateHistoryFromPoints(state, store);
  if (historyChanged && !saveStore(store)) {
    logf("Nord Pool moving average save failed");
  }

  float movingAvgKrPerKwh =
      store.count == 0 ? kDefaultMovingAverageKrPerKwh : movingAverageValue(store);
  if (movingAvgKrPerKwh <= 0.0001f) movingAvgKrPerKwh = kDefaultMovingAverageKrPerKwh;

  state.hasRunningAverage = true;
  state.runningAverage = movingAvgKrPerKwh;
  applyLevelsFromMovingAverage(state, movingAvgKrPerKwh);

  assignCurrentFromClock(state);
  if (state.currentIndex < 0) {
    state.currentIndex = 0;
    state.currentStartsAt = state.points[0].startsAt;
    state.currentPrice = state.points[0].price;
  }
  assignCurrentLevel(state);
  return store.count;
}
}  // namespace

void fetchNordPoolPriceInfo(
    const char *apiBaseUrl,
    const char *area,
    const char *currency,
    uint16_t resolutionMinutes,
    PriceState &out) {
  out.ok = false;
  out.error = "";
  out.source = "NORDPOOL";
  out.hasRunningAverage = false;
  out.runningAverage = 0.0f;
  out.currency = "SEK";
  out.resolutionMinutes = normalizeResolutionMinutes(resolutionMinutes);
  out.currentStartsAt = "";
  out.currentLevel = "UNKNOWN";
  out.currentPrice = 0.0f;
  out.currentIndex = -1;
  out.count = 0;
  logf("Nord Pool fetch start: resolution=%u free_heap=%u", (unsigned)out.resolutionMinutes, ESP.getFreeHeap());

  if (WiFi.status() != WL_CONNECTED) {
    out.error = "WiFi not connected";
    return;
  }

  const time_t now = time(nullptr);
  if (now < 1700000000) {
    out.error = "Clock not synced";
    return;
  }

  char today[16];
  char tomorrow[16];
  if (!formatDate(now, today, sizeof(today)) || !formatDate(now + 24 * 3600, tomorrow, sizeof(tomorrow))) {
    out.error = "Date format failed";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  if (!fetchDate(http, client, apiBaseUrl, today, area, currency, out.resolutionMinutes, out)) {
    return;
  }

  // Tomorrow can be unavailable earlier in the day; keep today's prices if present.
  if (!fetchDate(http, client, apiBaseUrl, tomorrow, area, currency, out.resolutionMinutes, out)) {
    logf("Nord Pool tomorrow fetch failed: %s", out.error.c_str());
    if (out.count == 0) {
      return;
    }
    out.error = "";
  }

  if (out.count == 0) {
    out.error = "No prices";
    return;
  }

  const uint16_t sampleCount = applyMovingAverageToState(out);

  out.ok = true;
  logf(
      "Nord Pool OK: points=%u res=%u current=%.3f %s level=%s ma=%.3f samples=%u",
      (unsigned)out.count,
      (unsigned)out.resolutionMinutes,
      out.currentPrice,
      out.currency.c_str(),
      out.currentLevel.c_str(),
      out.runningAverage,
      (unsigned)sampleCount
  );
}

void nordPoolPreupdateMovingAverageFromPriceInfo(PriceState &state) {
  if (state.source != "NORDPOOL" && state.source != "no wifi") return;
  if (!state.ok || state.count == 0) return;

  (void)applyMovingAverageToState(state);
}
