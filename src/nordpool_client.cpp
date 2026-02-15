#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "logging_utils.h"
#include "nordpool_client.h"
#include "time_utils.h"

namespace {
constexpr uint32_t kHttpTimeoutMs = 10000;
constexpr size_t kMovingAverageWindowHours = 72;
constexpr char kMovingAveragePath[] = "/nordpool_ma.bin";
constexpr float kDefaultMovingAverageKrPerKwh = 1.0f;
constexpr uint32_t kAvgStoreMagic = 0x4E504D41;  // "NPMA"
constexpr uint16_t kAvgStoreVersion = 1;

struct MovingAverageStore {
  uint32_t magic = kAvgStoreMagic;
  uint16_t version = kAvgStoreVersion;
  uint16_t count = 0;
  uint16_t head = 0;  // next write index
  char lastHourKey[14] = {0};  // YYYY-MM-DDTHH
  float values[kMovingAverageWindowHours] = {0.0f};
};

float applyCustomPriceFormula(float rawPriceKrPerKwh) {
  // Apply formula in ore: 1.25 * hourly_price + 84.225, then convert back to kr.
  const float rawOre = rawPriceKrPerKwh * 100.0f;
  const float adjustedOre = (1.25f * rawOre) + 84.225f;
  return adjustedOre / 100.0f;
}

String classifyLevel(float priceKrPerKwh) {
  (void)priceKrPerKwh;
  return "UNKNOWN";
}

bool formatDate(time_t ts, char *out, size_t outSize) {
  struct tm localTm;
  if (!localtime_r(&ts, &localTm)) return false;
  return strftime(out, outSize, "%Y-%m-%d", &localTm) > 0;
}

bool parseUtcIso(const String &iso, struct tm &tmUtc) {
  if (iso.length() < 19) return false;

  tmUtc = {};
  tmUtc.tm_year = iso.substring(0, 4).toInt() - 1900;
  tmUtc.tm_mon = iso.substring(5, 7).toInt() - 1;
  tmUtc.tm_mday = iso.substring(8, 10).toInt();
  tmUtc.tm_hour = iso.substring(11, 13).toInt();
  tmUtc.tm_min = iso.substring(14, 16).toInt();
  tmUtc.tm_sec = iso.substring(17, 19).toInt();

  return true;
}

bool isHourKey(const String &value) {
  return value.length() == 13;
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
  if (store.head >= kMovingAverageWindowHours) return false;
  if (store.count > kMovingAverageWindowHours) return false;
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
  store.values[store.head] = value;
  store.head = (store.head + 1) % kMovingAverageWindowHours;
  if (store.count < kMovingAverageWindowHours) ++store.count;
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

bool updateHistoryFromPoints(PriceState &state, MovingAverageStore &store, const String &nowKey) {
  if (!isHourKey(nowKey)) return false;

  bool changed = false;
  String lastPersisted = String(store.lastHourKey);
  for (size_t i = 0; i < state.count; ++i) {
    const String pointKey = hourKeyFromIso(state.points[i].startsAt);
    if (!isHourKey(pointKey)) continue;
    if (pointKey > nowKey) continue;              // future hour, ignore
    if (isHourKey(lastPersisted) && pointKey <= lastPersisted) continue;  // already processed

    addMovingAverageSample(store, state.points[i].price);
    strncpy(store.lastHourKey, pointKey.c_str(), sizeof(store.lastHourKey) - 1);
    store.lastHourKey[sizeof(store.lastHourKey) - 1] = '\0';
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

String utcIsoToLocalIsoHour(const String &utcIso) {
  struct tm tmUtc;
  if (!parseUtcIso(utcIso, tmUtc)) return utcIso;

  const time_t epochUtc = utcToEpochSeconds(tmUtc);
  if (epochUtc <= 0) return utcIso;

  struct tm localTm;
  if (!localtime_r(&epochUtc, &localTm)) return utcIso;

  char out[32];
  strftime(out, sizeof(out), "%Y-%m-%dT%H:00:00", &localTm);
  return String(out);
}

bool addPoints(JsonArray arr, const String &area, PriceState &state) {
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
    p.startsAt = utcIsoToLocalIsoHour(String((const char *)(item["deliveryStart"] | "")));
    p.price = adjustedPrice;
    p.level = classifyLevel(adjustedPrice);
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
    PriceState &out
) {
  const String url =
      String(apiBaseUrl) + "?date=" + String(date) + "&market=DayAhead&indexNames=" + String(area) +
      "&currency=" + String(currency) + "&resolutionInMinutes=60";

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

  const String payload = http.getString();
  if (payload.length() == 0) {
    out.error = "Empty response body";
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  http.end();
  if (err) {
    out.error = "JSON parse failed";
    const String preview = payload.substring(0, 120);
    logf("Nord Pool JSON parse error: %s body[0..120]=%s", err.c_str(), preview.c_str());
    return false;
  }

  if (!doc["title"].isNull() && String((const char *)(doc["title"] | "")) == "Unauthorized") {
    out.error = "Nord Pool API unauthorized";
    return false;
  }

  if (!doc["currency"].isNull()) {
    out.currency = String((const char *)(doc["currency"] | currency));
  }

  addPoints(doc["multiIndexEntries"], String(area), out);
  return true;
}

void assignCurrentFromClock(PriceState &out) {
  const String key = currentHourKey();
  if (key.isEmpty()) return;

  out.currentIndex = -1;
  for (size_t i = 0; i < out.count; ++i) {
    if (hourKeyFromIso(out.points[i].startsAt) == key) {
      out.currentIndex = (int)i;
      break;
    }
  }

  if (out.currentIndex < 0) return;

  const PricePoint &point = out.points[out.currentIndex];
  out.currentStartsAt = point.startsAt;
  out.currentPrice = point.price;
}

void assignCurrentLevel(PriceState &out) {
  if (out.currentIndex < 0 || out.currentIndex >= (int)out.count) return;
  out.currentLevel = out.points[out.currentIndex].level;
}
}  // namespace

PriceState fetchNordPoolPriceInfo(const char *apiBaseUrl, const char *area, const char *currency) {
  PriceState out;
  out.source = "NORDPOOL";
  logf("Nord Pool fetch start. free_heap=%u", ESP.getFreeHeap());

  if (WiFi.status() != WL_CONNECTED) {
    out.error = "WiFi not connected";
    return out;
  }

  const time_t now = time(nullptr);
  if (now < 1700000000) {
    out.error = "Clock not synced";
    return out;
  }

  char today[16];
  char tomorrow[16];
  if (!formatDate(now, today, sizeof(today)) || !formatDate(now + 24 * 3600, tomorrow, sizeof(tomorrow))) {
    out.error = "Date format failed";
    return out;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  if (!fetchDate(http, client, apiBaseUrl, today, area, currency, out)) {
    return out;
  }

  // Tomorrow can be unavailable earlier in the day; keep today's prices if present.
  if (!fetchDate(http, client, apiBaseUrl, tomorrow, area, currency, out)) {
    logf("Nord Pool tomorrow fetch failed: %s", out.error.c_str());
    if (out.count == 0) {
      return out;
    }
    out.error = "";
  }

  if (out.count == 0) {
    out.error = "No hourly prices";
    return out;
  }

  MovingAverageStore store;
  if (!loadStore(store)) {
    resetStore(store);
  }

  const String nowKey = currentHourKey();
  const bool historyChanged = updateHistoryFromPoints(out, store, nowKey);
  if (historyChanged && !saveStore(store)) {
    logf("Nord Pool moving average save failed");
  }

  float movingAvgKrPerKwh =
      store.count == 0 ? kDefaultMovingAverageKrPerKwh : movingAverageValue(store);
  if (movingAvgKrPerKwh <= 0.0001f) movingAvgKrPerKwh = kDefaultMovingAverageKrPerKwh;
  out.hasRunningAverage = true;
  out.runningAverage = movingAvgKrPerKwh;
  applyLevelsFromMovingAverage(out, movingAvgKrPerKwh);

  assignCurrentFromClock(out);
  if (out.currentIndex < 0) {
    out.currentIndex = 0;
    out.currentStartsAt = out.points[0].startsAt;
    out.currentPrice = out.points[0].price;
  }
  assignCurrentLevel(out);

  out.ok = true;
  logf(
      "Nord Pool OK: points=%u current=%.3f %s level=%s ma=%.3f samples=%u",
      (unsigned)out.count,
      out.currentPrice,
      out.currency.c_str(),
      out.currentLevel.c_str(),
      movingAvgKrPerKwh,
      (unsigned)store.count
  );
  return out;
}
