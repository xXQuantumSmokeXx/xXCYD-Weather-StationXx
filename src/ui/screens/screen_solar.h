#pragma once
#include <TFT_eSPI.h>

void screenSolarDraw(TFT_eSPI &tft, bool wifiOk);
void screenSolarTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);