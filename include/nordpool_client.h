#pragma once

#include "app_types.h"

PriceState fetchNordPoolPriceInfo(const char *apiBaseUrl, const char *area, const char *currency);
void nordPoolPreupdateMovingAverageFromPriceInfo(PriceState &state);
