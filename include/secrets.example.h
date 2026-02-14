#pragma once

// Copy this file to include/secrets.h and fill in your credentials.
// For electricity prices, use a token from developer.tibber.com (GraphQL API).
// The new Data API at data-api.tibber.com does not expose Tibber price data.

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// Price source selector.
// Set PRICE_SOURCE to PRICE_SOURCE_TIBBER or PRICE_SOURCE_NORDPOOL.
#define PRICE_SOURCE_TIBBER 0
#define PRICE_SOURCE_NORDPOOL 1
#define PRICE_SOURCE PRICE_SOURCE_TIBBER

// Nord Pool options (used when PRICE_SOURCE == PRICE_SOURCE_NORDPOOL).
#define NORDPOOL_AREA "SE3"
#define NORDPOOL_CURRENCY "SEK"

#define TIBBER_API_TOKEN "tibber-personal-access-token"
