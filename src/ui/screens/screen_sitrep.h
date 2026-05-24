#pragma once
#include <TFT_eSPI.h>

void screenSitrepDraw(TFT_eSPI &tft, bool wifiOk);
void screenSitrepTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
