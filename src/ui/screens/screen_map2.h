#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

// MAP 2 — CONUS weather field: rain / temp / wind / smoke (AQI).
// Reuses the MAP projection and basemap drawing from screen_map.cpp.
void screenMap2Draw(TFT_eSPI &tft, bool wifiOk);
bool screenMap2Tap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
// True when y falls in one of MAP 2's own tap controls: the top bar
// (refresh) or the layer bar.  The map body itself is inert.
bool screenMap2IsControlTap(int16_t y);

// Async fetch — called from Core 0 worker
bool wxMapFetch(bool wifiOk);
extern bool g_wxMapPending;
bool triggerWxMapFetch();
