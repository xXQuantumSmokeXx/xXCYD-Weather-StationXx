#pragma once
#include <TFT_eSPI.h>

void screenUsgsDraw(TFT_eSPI &tft, bool wifiOk);
void screenUsgsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenUsgsSwipe(int dir);

// Async fetch — called from Core 0 worker
bool usgsFetch(bool wifiOk);
extern bool g_usgsPending;
bool triggerUsgsFetch();
