#include "time_utils.h"

#include "app_types.h"
#include "logging_utils.h"

namespace {
constexpr char kTimezoneCetCest[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr char kTimezoneEetEest[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
}  // namespace

uint16_t normalizeResolutionMinutes(uint16_t resolutionMinutes) {
  if (resolutionMinutes == 15 || resolutionMinutes == 30 || resolutionMinutes == 60) {
    return resolutionMinutes;
  }
  return 60;
}

const char *timezoneSpecForNordpoolArea(const String &area) {
  if (area == "FI" || area == "EE" || area == "LV" || area == "LT") {
    return kTimezoneEetEest;
  }
  return kTimezoneCetCest;
}

String intervalKeyFromIso(const String &iso, uint16_t resolutionMinutes) {
  if (iso.length() < 13) return "";

  const uint16_t normalizedResolution = normalizeResolutionMinutes(resolutionMinutes);
  if (normalizedResolution >= 60 || iso.length() < 16) {
    return iso.substring(0, 13);
  }

  const int minute = iso.substring(14, 16).toInt();
  const int slotMinute = minute - (minute % normalizedResolution);
  const String hourPrefix = iso.substring(0, 13);
  char key[20];
  snprintf(key, sizeof(key), "%s:%02d", hourPrefix.c_str(), slotMinute);
  return String(key);
}

String currentIntervalKey(uint16_t resolutionMinutes) {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";

  struct tm localTm;
  localtime_r(&now, &localTm);

  const uint16_t normalizedResolution = normalizeResolutionMinutes(resolutionMinutes);
  const int slotMinute = localTm.tm_min - (localTm.tm_min % normalizedResolution);
  char key[20];
  if (normalizedResolution >= 60) {
    strftime(key, sizeof(key), "%Y-%m-%dT%H", &localTm);
  } else {
    char hourPrefix[20];
    strftime(hourPrefix, sizeof(hourPrefix), "%Y-%m-%dT%H", &localTm);
    snprintf(key, sizeof(key), "%s:%02d", hourPrefix, slotMinute);
  }
  return String(key);
}

int findPricePointIndexForInterval(const PriceState &state, const String &intervalKey, uint16_t resolutionMinutes) {
  if (intervalKey.isEmpty()) return -1;
  for (size_t i = 0; i < state.count; ++i) {
    if (intervalKeyFromIso(state.points[i].startsAt, resolutionMinutes) == intervalKey) {
      return (int)i;
    }
  }
  return -1;
}

int findCurrentPricePointIndex(const PriceState &state, uint16_t resolutionMinutes) {
  const String key = currentIntervalKey(resolutionMinutes);
  if (key.isEmpty()) return -1;
  return findPricePointIndexForInterval(state, key, resolutionMinutes);
}

String hourKeyFromIso(const String &iso) {
  return intervalKeyFromIso(iso, 60);
}

String currentHourKey() {
  return currentIntervalKey(60);
}

void syncClock(const char *timezoneSpec) {
  logf("Clock sync start: tz=%s", timezoneSpec ? timezoneSpec : "(null)");
  configTzTime(timezoneSpec, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 20; ++i) {
    if (time(nullptr) > 1700000000) break;
    delay(250);
  }
  logf("Clock sync status: now=%ld", (long)time(nullptr));
}

time_t scheduleNextDailyFetch(time_t now, int hour, int minute) {
  if (now < 1700000000) return 0;

  struct tm tmNow;
  localtime_r(&now, &tmNow);
  tmNow.tm_hour = hour;
  tmNow.tm_min = minute;
  tmNow.tm_sec = 0;

  time_t next = mktime(&tmNow);
  if (next <= now) next += 24 * 3600;
  return next;
}
