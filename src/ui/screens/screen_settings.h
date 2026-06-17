#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

void     screenSettingsDraw(TFT_eSPI &tft, bool wifiOk);
bool     screenSettingsTap(TFT_eSPI &tft, int16_t x, int16_t y);
bool     screenSettingsRefreshTapped();
bool     screenSettingsGetAutoRotate();
uint32_t screenSettingsGetAutoRotateMs();
bool     screenSettingsGetPageEnabled(int page);
bool     screenSettingsIsFavorite(int page);
int      screenSettingsGetNextRotatePage(int current);
int      screenSettingsGetSleepTimerSecs();  // 0=OFF, 15/30/60/300 seconds
bool     screenSettingsGetScheduleEnabled();  // daily sleep/wake schedule active
int      screenSettingsGetSleepHour();        // 0-23, hour to turn backlight off
int      screenSettingsGetWakeHour();         // 0-23, hour to turn backlight on
