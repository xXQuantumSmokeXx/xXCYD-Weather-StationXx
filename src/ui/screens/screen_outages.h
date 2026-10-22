#pragma once
#include <TFT_eSPI.h>

void screenOutagesDraw(TFT_eSPI &tft, bool wifiOk);
void screenOutagesTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenOutagesSwipe(int dir);

// Async fetch — called from Core 0 worker
bool outagesFetch(bool wifiOk);
extern bool g_outagesPending;
bool triggerOutagesFetch();
