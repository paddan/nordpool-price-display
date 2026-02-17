#pragma once

#include <Arduino.h>
#include <time.h>

struct PriceState;

uint16_t normalizeResolutionMinutes(uint16_t resolutionMinutes);
String hourKeyFromIso(const String &iso);
String currentHourKey();
String intervalKeyFromIso(const String &iso, uint16_t resolutionMinutes);
String currentIntervalKey(uint16_t resolutionMinutes);
int findPricePointIndexForInterval(const PriceState &state, const String &intervalKey, uint16_t resolutionMinutes);
int findCurrentPricePointIndex(const PriceState &state, uint16_t resolutionMinutes);
const char *timezoneSpecForNordpoolArea(const String &area);
void syncClock(const char *timezoneSpec);
time_t scheduleNextDailyFetch(time_t now, int hour, int minute);
