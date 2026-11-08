#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

void screenMapDraw(TFT_eSPI &tft, bool wifiOk);
bool screenMapTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
// True when y falls in one of the MAP screen's own tap controls: the top bar
// (refresh) or the layer bar.  The map body itself is inert.
bool screenMapIsControlTap(int16_t y);

// Shared map primitives — used by MAP 2 (screen_map2.cpp) so both pages
// project onto the same CONUS basemap.
void mapDrawBasemap(TFT_eSPI &tft);
bool mapProjectIn(float lon, float lat, int &x, int &y);
void mapDrawHome(TFT_eSPI &tft, int cx, int cy);

// Async fetches — called from Core 0 worker
bool mapQuakesFetch(bool wifiOk);
extern bool g_mapQuakesPending;
bool triggerMapQuakesFetch();

bool mapGridFetch(bool wifiOk);
extern bool g_mapGridPending;
bool triggerMapGridFetch();

bool mapStormsFetch(bool wifiOk);
extern bool g_mapStormsPending;
bool triggerMapStormsFetch();
