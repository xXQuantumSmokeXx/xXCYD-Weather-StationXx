#pragma once
#include <TFT_eSPI.h>

void screenMeteorsDraw(TFT_eSPI &tft, bool wifiOk);
void screenMeteorsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenMeteorsSwipe(int dir);

// Async fetch — called from Core 0 worker
bool meteorsFetch(bool wifiOk);
extern bool g_meteorsPending;
void triggerMeteorsFetch();
