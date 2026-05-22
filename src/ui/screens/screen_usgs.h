#pragma once
#include <TFT_eSPI.h>

void screenUsgsDraw(TFT_eSPI &tft, bool wifiOk);
void screenUsgsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);