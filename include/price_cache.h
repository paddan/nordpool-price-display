#pragma once

#include "app_types.h"

bool priceCacheSave(const PriceState &state);
bool priceCacheLoadIfCurrent(const char *expectedSource, PriceState &out);
