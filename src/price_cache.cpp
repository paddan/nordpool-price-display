#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <string.h>

#include "logging_utils.h"
#include "price_cache.h"
#include "time_utils.h"

namespace {
constexpr char kCachePath[] = "/price_cache.json";
constexpr int kCacheVersion = 1;

bool ensureSpiffsMounted() {
  static bool attempted = false;
  static bool mounted = false;
  if (!attempted) {
    attempted = true;
    mounted = SPIFFS.begin(true);
    logf("Price cache SPIFFS mount: %s", mounted ? "ok" : "failed");
  }
  return mounted;
}

int findCurrentIndex(const PriceState &state) {
  const String key = currentHourKey();
  if (key.isEmpty()) return -1;

  for (size_t i = 0; i < state.count; ++i) {
    if (hourKeyFromIso(state.points[i].startsAt) == key) {
      return (int)i;
    }
  }
  return -1;
}

void applyCurrentFromIndex(PriceState &state, int idx) {
  if (idx < 0 || idx >= (int)state.count) return;

  state.currentIndex = idx;
  state.currentStartsAt = state.points[idx].startsAt;
  state.currentLevel = state.points[idx].level;
  state.currentPrice = state.points[idx].price;
}
}  // namespace

bool priceCacheSave(const PriceState &state) {
  if (!state.ok || state.count == 0) return false;
  if (!ensureSpiffsMounted()) return false;

  JsonDocument doc;
  doc["version"] = kCacheVersion;
  doc["source"] = state.source;
  doc["currency"] = state.currency;
  doc["hasRunningAverage"] = state.hasRunningAverage;
  doc["runningAverage"] = state.runningAverage;

  JsonArray points = doc["points"].to<JsonArray>();
  for (size_t i = 0; i < state.count; ++i) {
    JsonObject item = points.add<JsonObject>();
    item["startsAt"] = state.points[i].startsAt;
    item["level"] = state.points[i].level;
    item["price"] = state.points[i].price;
  }

  File file = SPIFFS.open(kCachePath, FILE_WRITE);
  if (!file) {
    logf("Price cache save failed: open");
    return false;
  }

  if (serializeJson(doc, file) == 0) {
    file.close();
    logf("Price cache save failed: serialize");
    return false;
  }
  file.flush();
  file.close();
  return true;
}

bool priceCacheLoadIfCurrent(const char *expectedSource, PriceState &out) {
  out = PriceState();
  if (!ensureSpiffsMounted()) return false;

  File file = SPIFFS.open(kCachePath, FILE_READ);
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    logf("Price cache parse failed: %s", err.c_str());
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != kCacheVersion) return false;

  out.source = String((const char *)(doc["source"] | ""));
  if (expectedSource != nullptr && strlen(expectedSource) > 0 && out.source != String(expectedSource)) {
    return false;
  }

  out.currency = String((const char *)(doc["currency"] | "SEK"));
  out.hasRunningAverage = doc["hasRunningAverage"] | false;
  out.runningAverage = doc["runningAverage"] | 0.0f;

  JsonArray points = doc["points"].as<JsonArray>();
  if (points.isNull()) return false;

  for (JsonObject item : points) {
    if (out.count >= kMaxPoints) break;

    const String startsAt = String((const char *)(item["startsAt"] | ""));
    if (startsAt.length() == 0) continue;

    PricePoint &p = out.points[out.count++];
    p.startsAt = startsAt;
    p.level = String((const char *)(item["level"] | "UNKNOWN"));
    p.price = item["price"] | 0.0f;
  }

  if (out.count == 0) return false;

  const int idx = findCurrentIndex(out);
  if (idx < 0) {
    // Cache exists but does not cover current hour.
    return false;
  }

  applyCurrentFromIndex(out, idx);
  out.ok = true;
  return true;
}
