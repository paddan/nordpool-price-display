#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

#include "app_types.h"
#include "display_ui.h"
#include "logging_utils.h"
#include "nordpool_client.h"
#include "price_cache.h"
#include "time_utils.h"
#include "wifi_utils.h"

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint16_t kWifiPortalTimeoutSec = 60;
constexpr uint32_t kRetryOnErrorMs = 30000;
constexpr time_t kRetryDailyIfUnchangedSec = 10 * 60;
constexpr uint32_t kResetHoldMs = 2000;
constexpr uint32_t kResetPollIntervalMs = 50;
constexpr int kDailyFetchHour = 13;
constexpr int kDailyFetchMinute = 0;
constexpr char kNordPoolApiUrl[] = "https://dataportal-api.nordpoolgroup.com/api/DayAheadPriceIndices";
constexpr time_t kValidEpochMin = 1700000000;

#ifndef CONFIG_RESET_PIN
#define CONFIG_RESET_PIN -1
#endif

#ifndef CONFIG_RESET_ACTIVE_LEVEL
#define CONFIG_RESET_ACTIVE_LEVEL LOW
#endif

PriceState gState;
PriceState gFetchBuffer;
PriceState gCacheBuffer;
AppSecrets gSecrets;
uint32_t gLastFetchMs = 0;
time_t gNextDailyFetch = 0;
uint32_t gLastMinuteTick = 0;
bool gPendingCatchUpRecheck = false;
bool gNeedsOnlineInit = false;

constexpr int kConfigResetPin = CONFIG_RESET_PIN;
constexpr int kConfigResetActiveLevel = CONFIG_RESET_ACTIVE_LEVEL;

const char *activeSourceLabel()
{
  return "NORDPOOL";
}

bool resetButtonPressed()
{
  if (kConfigResetPin < 0)
    return false;
  return digitalRead(kConfigResetPin) == kConfigResetActiveLevel;
}

bool resetButtonHeld(uint32_t holdMs = kResetHoldMs)
{
  if (!resetButtonPressed())
    return false;

  uint32_t elapsed = 0;
  while (elapsed < holdMs)
  {
    if (!resetButtonPressed())
      return false;
    delay(kResetPollIntervalMs);
    elapsed += kResetPollIntervalMs;
  }
  return true;
}

void handleResetRequest()
{
  if (!resetButtonHeld())
    return;

  logf("Reset button held, clearing WiFi/config settings");
  wifiResetSettings();
  delay(250);
  ESP.restart();
}

void logNextFetch(time_t nextFetch)
{
  if (nextFetch == 0)
    return;
  struct tm tmNext;
  localtime_r(&nextFetch, &tmNext);
  char buf[24];
  strftime(buf, sizeof(buf), "%d/%m %H:%M", &tmNext);
  logf("Next daily fetch scheduled: %s", buf);
}

bool hasValidClock(time_t now)
{
  return now > kValidEpochMin;
}

void scheduleDailyFetch(time_t now)
{
  gNextDailyFetch = scheduleNextDailyFetch(now, kDailyFetchHour, kDailyFetchMinute);
  logNextFetch(gNextDailyFetch);
}

void syncClockForSelectedArea()
{
  const char *timezoneSpec = timezoneSpecForNordpoolArea(gSecrets.nordpoolArea);
  logf("Clock timezone selected: area=%s", gSecrets.nordpoolArea.c_str());
  syncClock(timezoneSpec);
}

String dateKeyFromTime(time_t when)
{
  if (!hasValidClock(when))
    return "";

  struct tm localTm;
  localtime_r(&when, &localTm);
  char key[11];
  strftime(key, sizeof(key), "%Y-%m-%d", &localTm);
  return String(key);
}

bool stateContainsDate(const PriceState &state, const String &dateKey)
{
  if (!state.ok || state.count == 0 || dateKey.length() != 10)
    return false;

  for (size_t i = 0; i < state.count; ++i)
  {
    if (state.points[i].startsAt.length() < 10)
      continue;
    if (state.points[i].startsAt.substring(0, 10) == dateKey)
      return true;
  }
  return false;
}

bool shouldCatchUpMissedDailyUpdate(time_t now, const PriceState &state)
{
  if (!hasValidClock(now))
    return false;

  struct tm tmToday;
  localtime_r(&now, &tmToday);
  tmToday.tm_hour = kDailyFetchHour;
  tmToday.tm_min = kDailyFetchMinute;
  tmToday.tm_sec = 0;
  const time_t todayFetchTime = mktime(&tmToday);
  if (todayFetchTime == (time_t)-1 || now < todayFetchTime)
    return false;

  struct tm tmTomorrow = tmToday;
  tmTomorrow.tm_mday += 1;
  tmTomorrow.tm_hour = 0;
  tmTomorrow.tm_min = 0;
  tmTomorrow.tm_sec = 0;
  const time_t tomorrow = mktime(&tmTomorrow);
  if (!hasValidClock(tomorrow))
    return false;

  const String tomorrowDate = dateKeyFromTime(tomorrow);
  if (tomorrowDate.isEmpty())
    return false;

  const bool hasTomorrow = stateContainsDate(state, tomorrowDate);
  if (!hasTomorrow)
  {
    logf("After %02d:%02d and cache is missing %s, catch-up fetch needed",
         kDailyFetchHour,
         kDailyFetchMinute,
         tomorrowDate.c_str());
  }
  return !hasTomorrow;
}

void applyFetchedState(const PriceState &fetched)
{
  if (fetched.ok)
  {
    gState = fetched;
    if (!priceCacheSave(gState))
    {
      logf("Price cache save failed");
    }
  }
  else if (gState.count > 0)
  {
    gState.error = fetched.error;
  }
  else
  {
    gState = fetched;
  }
  displayDrawPrices(gState);
  gLastFetchMs = millis();
}

void fetchAndRender()
{
  logf("Fetch+render start");
  fetchNordPoolPriceInfo(
      kNordPoolApiUrl,
      gSecrets.nordpoolArea.c_str(),
      gSecrets.nordpoolCurrency.c_str(),
      gSecrets.nordpoolResolutionMinutes,
      gFetchBuffer);
  applyFetchedState(gFetchBuffer);
  logf("Fetch+render done");
}

bool isSamePoint(const PricePoint &lhs, const PricePoint &rhs)
{
  return lhs.startsAt == rhs.startsAt && lhs.level == rhs.level && fabsf(lhs.price - rhs.price) < 0.0005f;
}

bool hasNewPriceInfo(const PriceState &fetched, const PriceState &current)
{
  if (!fetched.ok || fetched.count == 0)
    return false;
  if (!current.ok || current.count == 0)
    return true;
  if (fetched.count != current.count)
    return true;

  for (size_t i = 0; i < fetched.count; ++i)
  {
    if (!isSamePoint(fetched.points[i], current.points[i]))
      return true;
  }
  return false;
}

size_t dayCount(const PriceState &state)
{
  if (!state.ok || state.count == 0)
    return 0;

  size_t uniqueDays = 0;
  String lastDay = "";
  for (size_t i = 0; i < state.count; ++i)
  {
    if (state.points[i].startsAt.length() < 10)
      continue;
    const String day = state.points[i].startsAt.substring(0, 10);
    if (day != lastDay)
    {
      lastDay = day;
      ++uniqueDays;
    }
  }
  return uniqueDays;
}

bool wouldReduceCoverage(const PriceState &fetched, const PriceState &current)
{
  if (!fetched.ok || !current.ok || current.count == 0)
    return false;

  if (fetched.count < current.count)
    return true;

  return dayCount(fetched) < dayCount(current);
}

bool applyLoadedCacheState(const PriceState &cacheState, const char *cacheLabel, bool saveBackToCache)
{
  if (cacheState.resolutionMinutes != gSecrets.nordpoolResolutionMinutes)
  {
    logf(
        "Using %s cache with different resolution: cache=%u configured=%u",
        cacheLabel,
        (unsigned)cacheState.resolutionMinutes,
        (unsigned)gSecrets.nordpoolResolutionMinutes);
  }

  gState = cacheState;
  if (saveBackToCache && !priceCacheSave(gState))
  {
    logf("Price cache save failed");
  }

  displayDrawPrices(gState);
  logf("Loaded %s prices from cache: points=%u", cacheLabel, (unsigned)gState.count);
  gPendingCatchUpRecheck = true;
  return true;
}

void updateCurrentHourFromClock()
{
  if (!gState.ok || gState.count == 0)
    return;

  const int idx = findCurrentPricePointIndex(gState, gSecrets.nordpoolResolutionMinutes);
  if (idx < 0 || idx == gState.currentIndex)
    return;

  gState.currentIndex = idx;
  gState.currentStartsAt = gState.points[idx].startsAt;
  gState.currentLevel = gState.points[idx].level;
  gState.currentPrice = gState.points[idx].price;
  logf("Price slot update: idx=%d price=%.3f", idx, gState.currentPrice);
  displayDrawPrices(gState);
}

void handleClockDrivenUpdates(time_t now)
{
  if (!hasValidClock(now))
    return;

  if (gPendingCatchUpRecheck)
  {
    gPendingCatchUpRecheck = false;
    if (shouldCatchUpMissedDailyUpdate(now, gState))
    {
      gNextDailyFetch = now;
      logf("Delayed catch-up fetch scheduled immediately");
    }
  }

  const uint32_t minuteTick = (uint32_t)(now / 60);
  if (minuteTick != gLastMinuteTick)
  {
    gLastMinuteTick = minuteTick;
    updateCurrentHourFromClock();
  }

  if (gNextDailyFetch == 0)
    scheduleDailyFetch(now);

  if (gNextDailyFetch != 0 && now >= gNextDailyFetch)
  {
    logf("Daily 13:00 fetch trigger");
    fetchNordPoolPriceInfo(
        kNordPoolApiUrl,
        gSecrets.nordpoolArea.c_str(),
        gSecrets.nordpoolCurrency.c_str(),
        gSecrets.nordpoolResolutionMinutes,
        gFetchBuffer);
    const PriceState &fetched = gFetchBuffer;
    if (!fetched.ok)
    {
      logf("Daily fetch failed, retry in %ld sec", (long)kRetryDailyIfUnchangedSec);
      applyFetchedState(fetched);
      gNextDailyFetch = now + kRetryDailyIfUnchangedSec;
      logNextFetch(gNextDailyFetch);
      return;
    }

    if (wouldReduceCoverage(fetched, gState))
    {
      logf(
          "Daily fetch has fewer prices (%u < %u), keep existing and retry in %ld sec",
          (unsigned)fetched.count,
          (unsigned)gState.count,
          (long)kRetryDailyIfUnchangedSec);
      gNextDailyFetch = now + kRetryDailyIfUnchangedSec;
      logNextFetch(gNextDailyFetch);
      return;
    }

    if (hasNewPriceInfo(fetched, gState))
    {
      logf("Daily fetch returned updated prices");
      applyFetchedState(fetched);
      scheduleDailyFetch(now);
      return;
    }

    logf("Daily fetch unchanged, retry in %ld sec", (long)kRetryDailyIfUnchangedSec);
    gNextDailyFetch = now + kRetryDailyIfUnchangedSec;
    logNextFetch(gNextDailyFetch);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  logf("Boot");

  if (kConfigResetPin >= 0)
  {
    if (kConfigResetActiveLevel == LOW)
      pinMode(kConfigResetPin, INPUT_PULLUP);
    else
      pinMode(kConfigResetPin, INPUT_PULLDOWN);
  }

  handleResetRequest();

  displayInit();
  loadAppSecrets(gSecrets);

  bool loadedFromCache = false;
  const bool wifiConnected = wifiConnectWithConfigPortal(gSecrets, kWifiPortalTimeoutSec);

  if (!wifiConnected)
  {
    if (priceCacheLoadIfAvailable(activeSourceLabel(), gCacheBuffer))
    {
      gState = gCacheBuffer;
      gState.source = "no wifi";
      displayDrawPrices(gState);
      logf("No WiFi at boot, loaded prices from cache: points=%u", (unsigned)gState.count);
      gNeedsOnlineInit = true;
      return;
    }

    gState.ok = false;
    gState.source = "no wifi";
    gState.error = "no wifi";
    displayDrawPrices(gState);
    gNeedsOnlineInit = true;
    return;
  }

  syncClockForSelectedArea();
  const time_t nowAfterSync = time(nullptr);
  scheduleDailyFetch(nowAfterSync);

  if (priceCacheLoadIfCurrent(activeSourceLabel(), gCacheBuffer))
  {
    loadedFromCache = applyLoadedCacheState(gCacheBuffer, "current", true);
  }
  else if (priceCacheLoadIfAvailable(activeSourceLabel(), gCacheBuffer))
  {
    loadedFromCache = applyLoadedCacheState(gCacheBuffer, "available", false);
  }

  if (!loadedFromCache)
  {
    fetchAndRender();
  }

  const time_t now = time(nullptr);
  if (loadedFromCache && shouldCatchUpMissedDailyUpdate(now, gState))
  {
    gNextDailyFetch = now;
    logf("Startup catch-up fetch scheduled immediately");
    gPendingCatchUpRecheck = false;
  }
}

void loop()
{
  handleResetRequest();

  if (WiFi.status() != WL_CONNECTED && !wifiReconnect(kWifiConnectTimeoutMs))
  {
    if (gState.ok)
    {
      if (gState.source != "no wifi")
      {
        gState.source = "no wifi";
        displayDrawPrices(gState);
      }
    }
    else
    {
      const bool needsRedraw = gState.source != "no wifi" || gState.error != "no wifi";
      gState.source = "no wifi";
      gState.error = "no wifi";
      if (needsRedraw)
      {
        displayDrawPrices(gState);
      }
    }
    return;
  }

  if (gNeedsOnlineInit && WiFi.status() == WL_CONNECTED)
  {
    logf("WiFi restored, running online init");
    gNeedsOnlineInit = false;
    loadAppSecrets(gSecrets);
    syncClockForSelectedArea();
    scheduleDailyFetch(time(nullptr));
    fetchAndRender();
  }

  if (!gState.ok && millis() - gLastFetchMs >= kRetryOnErrorMs)
  {
    logf("Retry fetch due to error state");
    fetchAndRender();
  }

  handleClockDrivenUpdates(time(nullptr));
}
