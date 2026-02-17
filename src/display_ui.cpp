#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <OpenFontRender.h>
#include <TFT_eSPI.h>

#include "NotoSans_Bold.h"
#include "display_ui.h"
#include "logging_utils.h"

namespace
{
  TFT_eSPI tft;
  OpenFontRender ofr;
  bool gOpenFontReady = false;

  // Screen coordinate system:
  // - X grows to the right
  // - Y grows downward
  //
  // Quick tuning guide:
  // - Increase X => move right, decrease X => move left
  // - Increase Y => move down, decrease Y => move up
  // - Increase W/H/font size => make larger

  // Big price ("2.22 SEK") position and size.
  constexpr int kScreenCenterX = 160; // Center X for the big price text.
  constexpr int kPriceCenterY = 44;   // Center Y for the big price text.
  constexpr int kPriceFontSize = 86;  // Big price font size (OpenFontRender pixels).
  constexpr int kCurrencyFontSize = kPriceFontSize / 2;
  constexpr int kPriceCurrencyGapPx = 8;

  // Chart rectangle (outer graph area).
  constexpr int kChartX = 30;  // Left edge of graph area.
  constexpr int kChartY = 106; // Top edge of graph area.
  constexpr int kChartW = 286; // Graph width.
  constexpr int kChartH = 124; // Graph height.

  // Labels around the chart.
  constexpr int kDayLabelY = kChartY - 10; // Date labels above graph (14/02, 15/02).
  constexpr int kAxisLabelX = kChartX - 8; // Y-axis number labels to the left of graph.

  // Current-hour arrow marker.
  constexpr int kCurrentArrowHalfWidth = 4;          // Half arrow base width (full width = *2).
  constexpr int kCurrentArrowHeight = 13;            // Arrow height.
  constexpr uint16_t kCurrentArrowColor = TFT_WHITE; // Arrow color.

  // Axis font sizes (TFT_eSPI bitmap fonts).
  constexpr int kYAxisFontSize = 1;    // Y-axis labels/ticks text scale.
  constexpr int kTopXAxisFontSize = 1; // Day labels text scale.
  constexpr int kSourceLabelX = 316;
  constexpr int kSourceLabelY = 2;
  constexpr uint16_t kAverageLineColor = TFT_CYAN;

  void formatPriceValue(float value, char *out, size_t outSize)
  {
    if (outSize == 0)
      return;
    snprintf(out, outSize, "%.2f", value);
  }

  void formatCurrencyLabel(const String &currency, char *out, size_t outSize)
  {
    if (outSize == 0)
      return;

    const char *src = currency.c_str();
    const size_t len = strlen(src);
    size_t start = 0;
    while (start < len && isspace((unsigned char)src[start]))
      ++start;
    size_t end = len;
    while (end > start && isspace((unsigned char)src[end - 1]))
      --end;

    size_t j = 0;
    for (size_t i = start; i < end && j < (outSize - 1); ++i)
    {
      unsigned char c = (unsigned char)src[i];
      if (c >= 'a' && c <= 'z')
        c = (unsigned char)(c - ('a' - 'A'));
      out[j++] = (char)c;
    }
    out[j] = '\0';

    if (j == 0)
    {
      snprintf(out, outSize, "SEK");
    }
  }

  uint16_t levelColor(const String &level)
  {
    // 5-step gradient: light green -> dark red.
    if (level == "VERY_CHEAP" || level == "LOW")
      return tft.color565(170, 255, 170);  // light green
    if (level == "CHEAP")
      return tft.color565(96, 210, 110);   // medium green
    if (level == "NORMAL")
      return tft.color565(245, 190, 70);   // warm yellow/orange
    if (level == "EXPENSIVE" || level == "HIGH")
      return tft.color565(185, 55, 35);    // red
    if (level == "VERY_EXPENSIVE")
      return tft.color565(100, 0, 0);      // dark red
    return TFT_WHITE;
  }

  void hardResetController()
  {
#ifdef TFT_RST
#if (TFT_RST >= 0)
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(5);
    digitalWrite(TFT_RST, LOW);
    delay(20);
    digitalWrite(TFT_RST, HIGH);
    delay(150);
#endif
#endif
  }

  void drawPriceText(float priceValue, const String &currency, uint16_t color)
  {
    char priceText[16];
    char currencyText[8];
    formatPriceValue(priceValue, priceText, sizeof(priceText));
    formatCurrencyLabel(currency, currencyText, sizeof(currencyText));

    if (gOpenFontReady)
    {
      ofr.setFontColor(color, TFT_BLACK);
      ofr.setAlignment(Align::MiddleLeft);

      ofr.setFontSize(kPriceFontSize);
      const int priceWidth = (int)ofr.getTextWidth("%s", priceText);
      const int priceHeight = (int)ofr.getTextHeight("%s", priceText);
      ofr.setFontSize(kCurrencyFontSize);
      const int currencyWidth = (int)ofr.getTextWidth("%s", currencyText);
      const int currencyHeight = (int)ofr.getTextHeight("%s", currencyText);

      const int totalWidth = priceWidth + kPriceCurrencyGapPx + currencyWidth;
      const int startX = kScreenCenterX - (totalWidth / 2);
      const int priceY = kPriceCenterY;
      const int currencyY = kPriceCenterY + ((priceHeight - currencyHeight) / 2);

      ofr.setFontSize(kPriceFontSize);
      ofr.setCursor(startX, priceY);
      ofr.printf("%s", priceText);

      ofr.setFontSize(kCurrencyFontSize);
      ofr.setCursor(startX + priceWidth + kPriceCurrencyGapPx, currencyY);
      ofr.printf("%s", currencyText);
      return;
    }

    // Fallback if smooth font is unavailable.
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, TFT_BLACK);

    tft.setTextFont(4);
    tft.setTextSize(3);

    const int priceWidth = tft.textWidth(priceText);
    const int priceHeight = tft.fontHeight();

    tft.setTextFont(2);
    tft.setTextSize(2);
    const int currencyWidth = tft.textWidth(currencyText);
    const int currencyHeight = tft.fontHeight();

    const int totalWidth = priceWidth + kPriceCurrencyGapPx + currencyWidth;
    const int startX = kScreenCenterX - (totalWidth / 2);
    const int priceY = kPriceCenterY - (priceHeight / 2);
    const int currencyY = priceY + (priceHeight - currencyHeight);

    tft.setTextFont(4);
    tft.setTextSize(3);
    tft.drawString(priceText, startX, priceY);

    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.drawString(currencyText, startX + priceWidth + kPriceCurrencyGapPx, currencyY);

    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
  }

  struct ChartRange
  {
    float minPrice = 0.0f;
    float maxPrice = 1.0f;
    float span = 1.0f;
  };

  struct Rgb
  {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    Rgb() : r(0), g(0), b(0) {}
    Rgb(uint8_t rr, uint8_t gg, uint8_t bb) : r(rr), g(gg), b(bb) {}
  };

  struct LevelBand
  {
    bool has = false;
    float minPrice = 0.0f;
    float maxPrice = 0.0f;
  };

  static const Rgb kLevelColors[] = {
      Rgb(170, 255, 170),  // VERY_CHEAP / LOW
      Rgb(96, 210, 110),   // CHEAP
      Rgb(245, 190, 70),   // NORMAL
      Rgb(185, 55, 35),    // EXPENSIVE / HIGH
      Rgb(100, 0, 0),      // VERY_EXPENSIVE
  };

  int levelRank(const String &level)
  {
    if (level == "VERY_CHEAP" || level == "LOW")
      return 0;
    if (level == "CHEAP")
      return 1;
    if (level == "NORMAL")
      return 2;
    if (level == "EXPENSIVE" || level == "HIGH")
      return 3;
    if (level == "VERY_EXPENSIVE")
      return 4;
    return -1;
  }

  uint8_t lerpU8(uint8_t a, uint8_t b, float t)
  {
    if (t <= 0.0f)
      return a;
    if (t >= 1.0f)
      return b;
    const float af = (float)a;
    const float bf = (float)b;
    int value = (int)lroundf(af + ((bf - af) * t));
    if (value < 0)
      value = 0;
    if (value > 255)
      value = 255;
    return (uint8_t)value;
  }

  uint16_t lerpRgb565(const Rgb &from, const Rgb &to, float t)
  {
    const uint8_t r = lerpU8(from.r, to.r, t);
    const uint8_t g = lerpU8(from.g, to.g, t);
    const uint8_t b = lerpU8(from.b, to.b, t);
    return tft.color565(r, g, b);
  }

  Rgb lerpRgb(const Rgb &from, const Rgb &to, float t)
  {
    return Rgb(lerpU8(from.r, to.r, t), lerpU8(from.g, to.g, t), lerpU8(from.b, to.b, t));
  }

  float clamp01(float v)
  {
    if (v < 0.0f)
      return 0.0f;
    if (v > 1.0f)
      return 1.0f;
    return v;
  }

  void computeLevelBands(const PriceState &state, LevelBand bands[5])
  {
    for (size_t i = 0; i < state.count; ++i)
    {
      const int rank = levelRank(state.points[i].level);
      if (rank < 0 || rank > 4)
        continue;

      LevelBand &band = bands[rank];
      if (!band.has)
      {
        band.has = true;
        band.minPrice = state.points[i].price;
        band.maxPrice = state.points[i].price;
        continue;
      }
      if (state.points[i].price < band.minPrice)
        band.minPrice = state.points[i].price;
      if (state.points[i].price > band.maxPrice)
        band.maxPrice = state.points[i].price;
    }
  }

  uint16_t barGradientColor(const PricePoint &point, const LevelBand bands[5], const ChartRange &range)
  {
    const int rank = levelRank(point.level);
    if (rank < 0 || rank > 4 || !bands[rank].has)
    {
      // Fallback to global gradient if level is unknown.
      const float t = clamp01((point.price - range.minPrice) / range.span);
      constexpr int kSegments = (int)(sizeof(kLevelColors) / sizeof(kLevelColors[0])) - 1;
      const float scaled = t * (float)kSegments;
      int idx = (int)floorf(scaled);
      if (idx < 0)
        idx = 0;
      if (idx >= kSegments)
        idx = kSegments - 1;
      const float localT = scaled - (float)idx;
      return lerpRgb565(kLevelColors[idx], kLevelColors[idx + 1], localT);
    }

    const LevelBand &band = bands[rank];
    const float span = band.maxPrice - band.minPrice;
    if (span < 0.001f)
      return tft.color565(kLevelColors[rank].r, kLevelColors[rank].g, kLevelColors[rank].b);
    const float t = clamp01((point.price - band.minPrice) / span);

    // Keep color anchored in current level hue.
    // Only shift toward neighboring level hues when those levels are present.
    Rgb lowSide = kLevelColors[rank];
    Rgb highSide = kLevelColors[rank];
    constexpr float kLowerShift = 0.70f;
    constexpr float kHigherShift = 0.45f;

    if (rank > 0 && bands[rank - 1].has)
    {
      lowSide = lerpRgb(kLevelColors[rank], kLevelColors[rank - 1], kLowerShift);
    }
    if (rank < 4 && bands[rank + 1].has)
    {
      highSide = lerpRgb(kLevelColors[rank], kLevelColors[rank + 1], kHigherShift);
    }

    return lerpRgb565(lowSide, highSide, t);
  }

  ChartRange computeChartRange(const PriceState &state)
  {
    ChartRange range;
    if (state.count == 0)
      return range;

    range.minPrice = state.points[0].price;
    range.maxPrice = state.points[0].price;
    for (size_t i = 1; i < state.count; ++i)
    {
      if (state.points[i].price < range.minPrice)
        range.minPrice = state.points[i].price;
      if (state.points[i].price > range.maxPrice)
        range.maxPrice = state.points[i].price;
    }
    range.span = (range.maxPrice - range.minPrice);
    if (range.span < 0.001f)
      range.span = 0.001f;
    return range;
  }

  int priceToY(float price, const ChartRange &range, int xAxisY, int drawableH)
  {
    const float normalized = (price - range.minPrice) / range.span;
    return xAxisY - (int)(normalized * drawableH);
  }

  void drawErrorScreen(const String &errorText)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextFont(4);
    tft.drawString("Fetch failed", kScreenCenterX, 70);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextFont(2);
    tft.drawString(errorText, kScreenCenterX, 96);
  }

  void drawClockLabel()
  {
    char text[6] = "--:--";
    const time_t now = time(nullptr);
    if (now > 1700000000)
    {
      struct tm tmNow;
      if (localtime_r(&now, &tmNow))
      {
        strftime(text, sizeof(text), "%H:%M", &tmNow);
      }
    }

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextFont(4);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, kSourceLabelX, kSourceLabelY);
    tft.setTextDatum(TL_DATUM);
  }

  void drawYAxis(const ChartRange &range, int xAxisY, int drawableH)
  {
    tft.setTextFont(kYAxisFontSize);

    auto drawAxisValueLabel = [&](float value, int y)
    {
      char label[12];
      snprintf(label, sizeof(label), "%.1f", value);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(label, kChartX - 3, y);
    };

    const int yTop = xAxisY - drawableH;
    const int yBottom = xAxisY;

    const float halfStart = ceilf(range.minPrice * 2.0f) / 2.0f;
    const float halfEnd = floorf(range.maxPrice * 2.0f) / 2.0f;

    for (float tick = halfStart; tick <= halfEnd + 0.001f; tick += 0.5f)
    {
      const int yTick = priceToY(tick, range, xAxisY, drawableH);
      const bool isWhole = fabsf(tick - roundf(tick)) < 0.01f;
      const int tickLen = isWhole ? 6 : 3;
      tft.drawFastHLine(kChartX - tickLen, yTick, tickLen, TFT_DARKGREY);

      if (!isWhole)
        continue;
      if (abs(yTick - yBottom) < 8 || abs(yTick - yTop) < 8)
        continue;

      char label[8];
      snprintf(label, sizeof(label), "%d", (int)roundf(tick));
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(label, kAxisLabelX, yTick);
    }

    tft.drawFastHLine(kChartX - 8, yBottom, 8, TFT_DARKGREY);
    tft.drawFastHLine(kChartX - 8, yTop, 8, TFT_DARKGREY);
    drawAxisValueLabel(range.minPrice, yBottom);
    drawAxisValueLabel(range.maxPrice, yTop);
    tft.setTextDatum(TL_DATUM);
  }

  void drawRunningAverage(const PriceState &state, const ChartRange &range, int xAxisY, int drawableH)
  {
    if (!state.hasRunningAverage)
      return;

    int yAvg = priceToY(state.runningAverage, range, xAxisY, drawableH);
    if (yAvg < kChartY)
      yAvg = kChartY;
    if (yAvg > xAxisY)
      yAvg = xAxisY;

    for (int x = kChartX; x < (kChartX + kChartW); x += 6)
    {
      tft.drawFastHLine(x, yAvg, 3, kAverageLineColor);
    }

    char label[16];
    snprintf(label, sizeof(label), "%.1f", state.runningAverage);
    tft.setTextColor(kAverageLineColor, TFT_BLACK);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(label, kAxisLabelX, yAvg);
    tft.setTextDatum(TL_DATUM);
  }

  void drawCurrentArrow(int barX, int barW, int barY)
  {
    const int centerX = barX + (barW / 2);
    int tipY = barY - 1;
    if (tipY < (kChartY + 3))
      tipY = kChartY + 3;

    int baseY = tipY - kCurrentArrowHeight;
    if (baseY < (kChartY + 1))
    {
      baseY = kChartY + 1;
      tipY = baseY + kCurrentArrowHeight;
    }

    tft.fillTriangle(
        centerX - kCurrentArrowHalfWidth,
        baseY,
        centerX + kCurrentArrowHalfWidth,
        baseY,
        centerX,
        tipY,
        kCurrentArrowColor);
  }

  void drawBars(const PriceState &state, const ChartRange &range, const LevelBand bands[5], int xAxisY, int drawableH)
  {
    char lastDay[11] = {0};
    bool hasLastDay = false;
    const int pointCount = (int)state.count;
    for (size_t i = 0; i < state.count; ++i)
    {
      const PricePoint &p = state.points[i];
      const int x0 = kChartX + (((int)i * kChartW) / pointCount);
      const int x1 = kChartX + ((((int)i + 1) * kChartW) / pointCount);
      const int x = x0;
      const int w = max(1, x1 - x0);
      const int y = priceToY(p.price, range, xAxisY, drawableH);
      const int h = xAxisY - y + 1;

      if (h > 0)
      {
        tft.fillRect(x, y, w, h, barGradientColor(p, bands, range));
      }

      if ((int)i == state.currentIndex)
      {
        drawCurrentArrow(x, w, y);
      }

      if (p.startsAt.length() < 10)
        continue;
      const char *startsAt = p.startsAt.c_str();
      char dayKey[11];
      memcpy(dayKey, startsAt, 10);
      dayKey[10] = '\0';
      if (hasLastDay && strncmp(dayKey, lastDay, 10) == 0)
        continue;

      memcpy(lastDay, dayKey, sizeof(lastDay));
      hasLastDay = true;

      char dayText[6];
      dayText[0] = startsAt[8];
      dayText[1] = startsAt[9];
      dayText[2] = '/';
      dayText[3] = startsAt[5];
      dayText[4] = startsAt[6];
      dayText[5] = '\0';
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setTextFont(kTopXAxisFontSize);
      tft.drawString(dayText, x, kDayLabelY);
    }
  }

  void drawCenteredLine(const char *text, int y, int font, uint16_t color)
  {
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(font);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString((text != nullptr) ? text : "", kScreenCenterX, y);
  }

  void drawCenteredLine(const String &text, int y, int font, uint16_t color)
  {
    drawCenteredLine(text.c_str(), y, font, color);
  }
} // namespace

void displayInit()
{
  hardResetController();
  tft.init();
  tft.writecommand(0x11); // SLPOUT
  delay(120);
  tft.writecommand(0x29); // DISPON
  delay(20);
  tft.setRotation(1);
  ofr.setDrawer(tft);
  ofr.setBackgroundFillMethod(BgFillMethod::Block);
  gOpenFontReady = (ofr.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)) == 0);
  logf("Display OpenFontRender: %s", gOpenFontReady ? "ready" : "fallback");
}

void displayDrawPrices(const PriceState &state)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  drawClockLabel();

  if (!state.ok)
  {
    drawErrorScreen(state.error);
    return;
  }

  if (state.count == 0)
  {
    drawPriceText(state.currentPrice, state.currency, levelColor(state.currentLevel));
    tft.setTextDatum(TL_DATUM);
    return;
  }

  const int xAxisY = kChartY + kChartH - 1;
  const int drawableH = kChartH - 4;
  const ChartRange range = computeChartRange(state);
  LevelBand bands[5];
  computeLevelBands(state, bands);

  uint16_t currentPriceColor = levelColor(state.currentLevel);
  if (state.currentIndex >= 0 && state.currentIndex < (int)state.count)
  {
    currentPriceColor = barGradientColor(state.points[state.currentIndex], bands, range);
  }
  drawPriceText(state.currentPrice, state.currency, currentPriceColor);
  tft.setTextDatum(TL_DATUM);

  tft.drawRect(kChartX - 1, kChartY - 1, kChartW + 2, kChartH + 2, TFT_DARKGREY);
  tft.drawFastHLine(kChartX, xAxisY, kChartW, TFT_DARKGREY);
  drawYAxis(range, xAxisY, drawableH);
  drawBars(state, range, bands, xAxisY, drawableH);
  drawRunningAverage(state, range, xAxisY, drawableH);
}

void displayDrawWifiConfigPortal(const char *apName, uint16_t timeoutSeconds)
{
  const char *ap = (apName != nullptr && apName[0] != '\0') ? apName : "ElMeter";
  char timeoutBuf[24];
  snprintf(timeoutBuf, sizeof(timeoutBuf), "Portal timeout: %us", (unsigned)timeoutSeconds);

  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);
  drawCenteredLine("Wi-Fi Setup Mode", 20, 4, TFT_CYAN);
  drawCenteredLine("1) Connect phone/computer to:", 58, 2, TFT_LIGHTGREY);
  drawCenteredLine(ap, 80, 2, TFT_WHITE);
  drawCenteredLine("2) Open: 192.168.4.1", 108, 2, TFT_LIGHTGREY);
  drawCenteredLine("3) Select Wi-Fi and Save", 130, 2, TFT_LIGHTGREY);
  drawCenteredLine("4) Select Nord Pool area,", 152, 2, TFT_LIGHTGREY);
  drawCenteredLine("   currency, and resolution", 170, 2, TFT_LIGHTGREY);
  drawCenteredLine(timeoutBuf, 194, 2, TFT_YELLOW);
}

void displayDrawWifiConfigTimeout(uint16_t timeoutSeconds)
{
  char timeoutBuf[40];
  snprintf(timeoutBuf, sizeof(timeoutBuf), "Timed out after %us", (unsigned)timeoutSeconds);

  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);
  drawCenteredLine("Wi-Fi Setup Timed Out", 74, 4, TFT_RED);
  drawCenteredLine(timeoutBuf, 108, 2, TFT_LIGHTGREY);
  drawCenteredLine("Press reset or reboot to retry", 136, 2, TFT_LIGHTGREY);
}
