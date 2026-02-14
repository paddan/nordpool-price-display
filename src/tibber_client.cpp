#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "logging_utils.h"
#include "tibber_client.h"
#include "time_utils.h"

namespace {
float applyCustomPriceFormula(float rawPriceKrPerKwh) {
  // Apply formula in ore: 1.25 * hourly_price + 84.225, then convert back to kr.
  const float rawOre = rawPriceKrPerKwh * 100.0f;
  const float adjustedOre = (1.25f * rawOre) + 84.225f;
  return adjustedOre / 100.0f;
}

constexpr uint32_t kHttpTimeoutMs = 10000;

static const char kPriceInfoQueryBody[] =
    "{\"query\":\"{viewer{homes{currentSubscription{priceInfo{current{energy startsAt currency "
    "level} today{energy startsAt level} tomorrow{energy startsAt level}}}}}}\"}";

JsonObject getPriceInfoNode(JsonDocument &doc) {
  return doc["data"]["viewer"]["homes"][0]["currentSubscription"]["priceInfo"];
}

int findCurrentIndex(const PriceState &state, const String &currentStartsAt) {
  for (size_t i = 0; i < state.count; ++i) {
    if (state.points[i].startsAt == currentStartsAt) {
      return (int)i;
    }
  }

  const String key = currentHourKey();
  for (size_t i = 0; i < state.count; ++i) {
    if (hourKeyFromIso(state.points[i].startsAt) == key) {
      return (int)i;
    }
  }
  return -1;
}
}  // namespace

void addPoints(JsonArray arr, PriceState &state) {
  if (arr.isNull()) return;
  for (JsonObject item : arr) {
    if (state.count >= kMaxPoints) return;
    PricePoint &p = state.points[state.count++];
    p.startsAt = String((const char *)(item["startsAt"] | ""));
    p.level = String((const char *)(item["level"] | "UNKNOWN"));
    const float rawPrice = item["energy"] | 0.0f;
    p.price = applyCustomPriceFormula(rawPrice);
  }
}

PriceState fetchPriceInfo(const char *apiToken, const char *graphQlUrl) {
  PriceState out;
  out.source = "TIBBER";
  logf("PriceInfo fetch start. free_heap=%u", ESP.getFreeHeap());

  if (apiToken == nullptr || strlen(apiToken) == 0) {
    out.error = "Missing TIBBER_API_TOKEN";
    return out;
  }
  if (WiFi.status() != WL_CONNECTED) {
    out.error = "WiFi not connected";
    return out;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, graphQlUrl)) {
    out.error = "HTTP begin failed";
    return out;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(apiToken));

  const int status = http.POST(String(kPriceInfoQueryBody));
  logf("Tibber POST status=%d", status);
  if (status != 200) {
    out.error = status <= 0 ? "HTTP POST failed" : ("HTTP " + String(status));
    http.end();
    return out;
  }

  JsonDocument doc;
  WiFiClient *stream = http.getStreamPtr();
  const DeserializationError err = deserializeJson(doc, *stream);
  http.end();
  if (err) {
    out.error = "JSON parse failed";
    logf("JSON parse error: %s", err.c_str());
    return out;
  }
  if (!doc["errors"].isNull()) {
    out.error = "Tibber API error";
    return out;
  }

  JsonObject priceInfo = getPriceInfoNode(doc);
  if (priceInfo.isNull()) {
    out.error = "No price info";
    return out;
  }

  JsonObject current = priceInfo["current"];
  if (current.isNull()) {
    out.error = "No current tariff";
    return out;
  }

  out.currency = String((const char *)(current["currency"] | "SEK"));
  out.currentStartsAt = String((const char *)(current["startsAt"] | ""));
  out.currentLevel = String((const char *)(current["level"] | "UNKNOWN"));
  const float rawCurrentPrice = current["energy"] | 0.0f;
  out.currentPrice = applyCustomPriceFormula(rawCurrentPrice);

  addPoints(priceInfo["today"], out);
  addPoints(priceInfo["tomorrow"], out);

  if (out.count == 0) {
    out.error = "No hourly prices";
    return out;
  }

  out.currentIndex = findCurrentIndex(out, out.currentStartsAt);

  out.ok = true;
  logf(
      "PriceInfo OK: points=%u current=%.3f %s level=%s",
      (unsigned)out.count,
      out.currentPrice,
      out.currency.c_str(),
      out.currentLevel.c_str()
  );
  return out;
}
