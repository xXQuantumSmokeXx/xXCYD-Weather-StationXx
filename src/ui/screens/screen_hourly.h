#pragma once
#include <TFT_eSPI.h>

void screenHourlyDraw(TFT_eSPI &tft, bool wifiOk);
bool screenHourlyTap(int16_t x, int16_t y);
