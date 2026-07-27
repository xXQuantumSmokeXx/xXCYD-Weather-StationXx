#pragma once
#include <TFT_eSPI.h>

void screenVolcanoesDraw(TFT_eSPI &tft, bool wifiOk);
void screenVolcanoesTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenVolcanoesSwipe(int dir);

// Async fetch — called from Core 0 worker
bool volcanoesFetch(bool wifiOk);
extern bool g_volcanoesPending;
bool triggerVolcanoesFetch();
