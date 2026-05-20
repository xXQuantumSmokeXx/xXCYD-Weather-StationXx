#pragma once
#include <TFT_eSPI.h>

void screenSettingsDraw(TFT_eSPI &tft, bool wifiOk);
bool screenSettingsTap(TFT_eSPI &tft, int16_t x, int16_t y);
bool screenSettingsRefreshTapped();
