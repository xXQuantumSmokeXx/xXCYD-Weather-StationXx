#pragma once
#include <TFT_eSPI.h>

void screenPlannerDraw(TFT_eSPI &tft, bool wifiOk);
void screenPlannerTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
