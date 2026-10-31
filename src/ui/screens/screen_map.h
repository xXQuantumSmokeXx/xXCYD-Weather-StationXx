#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

void screenMapDraw(TFT_eSPI &tft, bool wifiOk);
bool screenMapTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);

// Async fetch — called from Core 0 worker
bool mapQuakesFetch(bool wifiOk);
extern bool g_mapQuakesPending;
bool triggerMapQuakesFetch();
