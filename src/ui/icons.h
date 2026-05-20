#pragma once
#include <TFT_eSPI.h>
#include "../modules/weather.h"

void drawWeatherIcon(TFT_eSPI &tft, WxIcon icon, int cx, int cy, int r, uint16_t col);

inline void drawWmoIcon(TFT_eSPI &tft, int code, int cx, int cy, int r, uint16_t col) {
    drawWeatherIcon(tft, wmoIcon(code), cx, cy, r, col);
}
