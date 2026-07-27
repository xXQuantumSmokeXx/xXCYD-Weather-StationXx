#pragma once
#include <TFT_eSPI.h>

void screenEwsDraw(TFT_eSPI &tft, bool wifiOk);
void screenEwsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);

// Async fetch — called from Core 0 worker
bool ewsFetch(bool wifiOk);
extern bool g_ewsPending;
bool triggerEwsFetch();
