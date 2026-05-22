#pragma once
#include <TFT_eSPI.h>

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk);
void screenFiresTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);