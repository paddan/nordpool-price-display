#pragma once

#include "app_types.h"

void displayInit();
void displayDrawPrices(const PriceState &state);
void displayDrawWifiConfigPortal(const char *apName, uint16_t timeoutSeconds);
void displayDrawWifiConfigTimeout(uint16_t timeoutSeconds);
