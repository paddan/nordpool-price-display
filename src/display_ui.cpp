#include <Arduino.h>
#include <math.h>
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

  // Big price ("2.22 kr") position and size.
  constexpr int kScreenCenterX = 160; // Center X for the big price text.
  constexpr int kPriceCenterY = 44;   // Center Y for the big price text.
  constexpr int kPriceFontSize = 86;  // Big price font size (OpenFontRender pixels).

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

  String formatPrice(float value)
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f kr", value);
    return String(buf);
  }

  uint16_t levelColor(const String &level)
  {
    // Requested grouping:
    // LOW -> green, NORMAL -> yellow, HIGH -> red.
    if (level == "HIGH" || level == "VERY_EXPENSIVE" || level == "EXPENSIVE")
      return TFT_RED;
    if (level == "NORMAL")
      return TFT_YELLOW;
    if (level == "LOW" || level == "VERY_CHEAP" || level == "CHEAP")
      return TFT_GREEN;
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

  void drawPriceText(const String &priceText, uint16_t color)
  {
    if (gOpenFontReady)
    {
      ofr.setFontColor(color, TFT_BLACK);
      ofr.setFontSize(kPriceFontSize);
      ofr.setAlignment(Align::MiddleCenter);
      ofr.setCursor(kScreenCenterX, kPriceCenterY);
      ofr.printf("%s", priceText.c_str());
      return;
    }

    // Fallback if smooth font is unavailable.
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextFont(4);
    tft.setTextSize(3);
    tft.drawString(priceText, kScreenCenterX, kPriceCenterY);
    tft.setTextSize(1);
  }

  struct ChartRange
  {
    float minPrice = 0.0f;
    float maxPrice = 1.0f;
    float span = 1.0f;
  };

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

  void drawYAxis(const ChartRange &range, int xAxisY, int drawableH)
  {
    tft.setTextFont(kYAxisFontSize);

    auto drawAxisValueLabel = [&](float value, int y)
    {
      char label[12];
      snprintf(label, sizeof(label), "%.1f", value);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setTextDatum(MR_DATUM);
      tft.drawString(String(label), kChartX - 3, y);
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
      tft.drawString(String(label), kAxisLabelX, yTick);
    }

    tft.drawFastHLine(kChartX - 8, yBottom, 8, TFT_DARKGREY);
    tft.drawFastHLine(kChartX - 8, yTop, 8, TFT_DARKGREY);
    drawAxisValueLabel(range.minPrice, yBottom);
    drawAxisValueLabel(range.maxPrice, yTop);
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

  void drawBars(const PriceState &state, const ChartRange &range, int xAxisY, int drawableH)
  {
    String lastDay = "";
    const int pointCount = (int)state.count;
    for (size_t i = 0; i < state.count; ++i)
    {
      const PricePoint &p = state.points[i];
      const int x0 = kChartX + (((int)i * kChartW) / pointCount);
      const int x1 = kChartX + ((((int)i + 1) * kChartW) / pointCount);
      const int x = x0;
      const int w = max(1, x1 - x0 - 1);
      const int y = priceToY(p.price, range, xAxisY, drawableH);
      const int h = xAxisY - y + 1;

      if (h > 0)
      {
        tft.fillRect(x, y, w, h, levelColor(p.level));
      }

      if ((int)i == state.currentIndex)
      {
        drawCurrentArrow(x, w, y);
      }

      if (p.startsAt.length() < 10)
        continue;
      const String day = p.startsAt.substring(0, 10);
      if (day == lastDay)
        continue;

      lastDay = day;
      const String dayText = p.startsAt.substring(8, 10) + "/" + p.startsAt.substring(5, 7);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.setTextFont(kTopXAxisFontSize);
      tft.drawString(dayText, x, kDayLabelY);
    }
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

  if (!state.ok)
  {
    drawErrorScreen(state.error);
    return;
  }

  drawPriceText(formatPrice(state.currentPrice), levelColor(state.currentLevel));
  tft.setTextDatum(TL_DATUM);

  if (state.count == 0)
    return;

  const int xAxisY = kChartY + kChartH - 1;
  const int drawableH = kChartH - 4;
  const ChartRange range = computeChartRange(state);

  tft.drawRect(kChartX - 1, kChartY - 1, kChartW + 2, kChartH + 2, TFT_DARKGREY);
  tft.drawFastHLine(kChartX, xAxisY, kChartW, TFT_DARKGREY);
  drawYAxis(range, xAxisY, drawableH);
  drawBars(state, range, xAxisY, drawableH);
}
