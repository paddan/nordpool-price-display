# elprismatare_tibber

ESP32 + 2.4" TFT electricity price display for Tibber or Nord Pool.

This project runs on a FireBeetle ESP32 and shows:
- Current price as large text (`#.## kr`) with color based on price level.
- Hourly price bars for today + tomorrow.
- Current hour with a white downward arrow marker.

## Hardware

- ESP32 board: `FireBeetle-ESP32` (`board = firebeetle32`)
- Display: 2.4" 240x320 TFT shield (ILI9341-style, 8-bit parallel bus)

Wiring is documented in `WIRING.md`.

## Software Stack

- PlatformIO
- Arduino framework
- `TFT_eSPI`
- `ArduinoJson`
- Tibber GraphQL API (`https://api.tibber.com/v1-beta/gql`)
- Nord Pool Data Portal API (`https://dataportal-api.nordpoolgroup.com/api/DayAheadPriceIndices`)

## Configuration

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Fill in:
- `WIFI_SSID`
- `WIFI_PASSWORD`
- `PRICE_SOURCE` (`PRICE_SOURCE_TIBBER` or `PRICE_SOURCE_NORDPOOL`)
- `NORDPOOL_AREA` and `NORDPOOL_CURRENCY` when using Nord Pool
- `TIBBER_API_TOKEN` when using Tibber

`include/secrets.h` is ignored by git and must stay local.

## Build And Upload

```bash
platformio run -e wemos_d1_mini32_tft
platformio run -e wemos_d1_mini32_tft -t upload
platformio device monitor -b 115200
```

## Runtime Behavior

- Connects to Wi-Fi at boot.
- Syncs time via NTP (`CET/CEST` timezone).
- Fetches price data from the configured provider at startup.
- Refreshes hourly state from local clock.
- Fetches full price data again daily at 13:00 local time.
- Retries every 30 seconds on fetch failure.
- Applies custom price calculation: `1.25 * energy + 0.84225` (kr/kWh).
- Nord Pool level mapping uses Tibber-style bands against a 72-hour moving average persisted in SPIFFS (`/nordpool_ma.bin`).

## Project Structure

- `src/main.cpp`: app flow and scheduling
- `src/display_ui.cpp`: TFT rendering
- `src/tibber_client.cpp`: Tibber API client
- `src/nordpool_client.cpp`: Nord Pool API client
- `src/wifi_utils.cpp`: Wi-Fi helper
- `src/time_utils.cpp`: time/date helpers
- `src/logging_utils.cpp`: serial logging
- `include/*.h`: shared types and interfaces

## Notes

- Display reset uses `LCD_RST` on `GPIO27` (software-controlled reset pulse).
- `LCD_RD` must be held HIGH at 3.3V.
- If display remains white, verify wiring continuity and driver selection in `platformio.ini`.
