#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

// city = left side (location); screenLabel = centered screen name
void drawTopbar(TFT_eSPI &tft, const char *city, const char *screenLabel, const char *timeStr, bool wifiOk);
void drawBottombar(TFT_eSPI &tft, const char *updateStr, int activeScreen, int totalScreens);
void drawDivider(TFT_eSPI &tft, int y, uint16_t col = 0);
void drawStat(TFT_eSPI &tft, int x, int y, const char *label, const char *value);
void drawPrecipChip(TFT_eSPI &tft, int x, int y, int pct);
