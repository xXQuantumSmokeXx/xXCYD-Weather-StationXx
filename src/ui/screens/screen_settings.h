#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

void     screenSettingsDraw(TFT_eSPI &tft, bool wifiOk);
bool     screenSettingsTap(TFT_eSPI &tft, int16_t x, int16_t y);
bool     screenSettingsRefreshTapped();
bool     screenSettingsGetAutoRotate();
uint32_t screenSettingsGetAutoRotateMs();
bool     screenSettingsGetPageEnabled(int page);
int      screenSettingsGetNextRotatePage(int current);
