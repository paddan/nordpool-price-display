# FireBeetle ESP32 to 2.4" TFT Shield Wiring

This project is configured for a FireBeetle ESP32 board and an 8-bit parallel TFT
(ILI9341-style UNO shield pinout). Use jumper wires; this shield does not plug directly
into ESP32 headers.

The AZDelivery listing says "SPI", but this shield layout (`LCD_D0..D7`, `LCD_WR`, `LCD_RD`)
is a parallel TFT interface.

## Power

- `ESP32 GND` -> `GND`
- `ESP32 5V` (or USB 5V/VIN pin) -> `5V`

Leave shield `3V3` unconnected for power input.

Do not use 5V logic on ESP32 signal pins.

## TFT control/data pins

- `GPIO14` (`A11`) -> `LCD_CS`
- `GPIO13` (`D7`) -> `LCD_RS` (D/C)
- `GPIO27` -> `LCD_RST` (software reset enabled)
- `GPIO22` (`SCL`) -> `LCD_WR`
- `ESP32 3V3` -> `LCD_RD` (fixed HIGH, write-only mode)
- `GPIO26` (`D3`) -> `LCD_D0`
- `GPIO25` (`D2`) -> `LCD_D1`
- `GPIO21` (`SDA`) -> `LCD_D2`
- `GPIO23` (`MOSI`) -> `LCD_D3`
- `GPIO19` (`MISO`) -> `LCD_D4`
- `GPIO18` (`SCK`) -> `LCD_D5`
- `GPIO5` (`D8`) -> `LCD_D6`
- `GPIO15` (`A4`) -> `LCD_D7`

## Notes

- These pins match `platformio.ini` exactly. If you change wiring, update the `TFT_*` build flags too.
- `GPIO32`/`GPIO33` are not used because they are not exposed on many FireBeetle layouts.
- Do not use `GPIO0`/`GPIO2` for this TFT bus on FireBeetle; they are boot strapping pins and can cause reset loops.
- Avoid wiring TFT to `GPIO1`/`GPIO3` (UART TX/RX), or flashing can fail with "serial TX path seems down".
- `LCD_RST` is connected to ESP32 `GPIO27`, so TFT_eSPI can reset the panel in software (`tft.init()` toggles reset).
- `LCD_RD` must be held HIGH at 3.3V. If your shield has no 3.3V pin, use ESP32 `3V3`.
- If the screen stays white, the controller may not be ILI9341. In that case change `ILI9341_DRIVER` in `platformio.ini` to the matching TFT_eSPI driver.
- SD card pins on this shield are not configured yet.

## Optional Reset Button (Wi-Fi/config reset)

You can wire a button to clear saved Wi-Fi and Nord Pool settings:

- One side of button -> configured `CONFIG_RESET_PIN`
- Other side -> `GND` (if `CONFIG_RESET_ACTIVE_LEVEL=LOW`) or `3V3` (if `HIGH`)

Default project config has `CONFIG_RESET_PIN=-1` (disabled). Set a free GPIO in `platformio.ini` to enable it.
When enabled, hold the button for 2 seconds during runtime or boot to clear saved config and restart.
