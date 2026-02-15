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

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h. Copy include/secrets.example.h to include/secrets.h and set credentials."
#endif

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kRetryOnErrorMs = 30000;
constexpr time_t kRetryDailyIfUnchangedSec = 10 * 60;
constexpr char kNordPoolApiUrl[] = "https://dataportal-api.nordpoolgroup.com/api/DayAheadPriceIndices";
constexpr char kTimezoneSpec[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr time_t kValidEpochMin = 1700000000;

#ifndef NORDPOOL_AREA
#define NORDPOOL_AREA "SE3"
#endif

#ifndef NORDPOOL_CURRENCY
#define NORDPOOL_CURRENCY "SEK"
#endif

PriceState gState;
uint32_t gLastFetchMs = 0;
time_t gNextDailyFetch = 0;
uint32_t gLastMinuteTick = 0;

const char *activeSourceLabel()
{
  return "NORDPOOL";
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
  gNextDailyFetch = scheduleNextDailyFetch(now, 13, 0);
  logNextFetch(gNextDailyFetch);
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
  applyFetchedState(fetchNordPoolPriceInfo(kNordPoolApiUrl, NORDPOOL_AREA, NORDPOOL_CURRENCY));
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

void updateCurrentHourFromClock()
{
  if (!gState.ok || gState.count == 0)
    return;

  const String key = currentHourKey();
  if (key.isEmpty())
    return;

  int idx = -1;
  for (size_t i = 0; i < gState.count; ++i)
  {
    if (hourKeyFromIso(gState.points[i].startsAt) == key)
    {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0 || idx == gState.currentIndex)
    return;

  gState.currentIndex = idx;
  gState.currentStartsAt = gState.points[idx].startsAt;
  gState.currentLevel = gState.points[idx].level;
  gState.currentPrice = gState.points[idx].price;
  logf("Hour change update: idx=%d price=%.3f", idx, gState.currentPrice);
  displayDrawPrices(gState);
}

void handleClockDrivenUpdates(time_t now)
{
  if (!hasValidClock(now))
    return;

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
    const PriceState fetched = fetchNordPoolPriceInfo(kNordPoolApiUrl, NORDPOOL_AREA, NORDPOOL_CURRENCY);
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

  displayInit();

  if (!wifiConnect(WIFI_SSID, WIFI_PASSWORD, kWifiConnectTimeoutMs))
  {
    gState.ok = false;
    gState.source = activeSourceLabel();
    gState.error = "WiFi timeout";
    displayDrawPrices(gState);
    return;
  }

  syncClock(kTimezoneSpec);
  scheduleDailyFetch(time(nullptr));

  PriceState cached;
  if (priceCacheLoadIfCurrent(activeSourceLabel(), cached))
  {
    nordPoolPreupdateMovingAverageFromPriceInfo(cached);
    gState = cached;
    if (!priceCacheSave(gState))
    {
      logf("Price cache save failed");
    }
    displayDrawPrices(gState);
    logf("Loaded current prices from cache: points=%u", (unsigned)gState.count);
    return;
  }

  fetchAndRender();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED && !wifiConnect(WIFI_SSID, WIFI_PASSWORD, kWifiConnectTimeoutMs))
  {
    return;
  }

  if (!gState.ok && millis() - gLastFetchMs >= kRetryOnErrorMs)
  {
    logf("Retry fetch due to error state");
    fetchAndRender();
  }

  handleClockDrivenUpdates(time(nullptr));
}
