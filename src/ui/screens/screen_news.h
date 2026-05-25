#pragma once
#include <TFT_eSPI.h>

void screenNewsDraw(TFT_eSPI &tft, bool wifiOk);
void screenNewsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenNewsSwipe(int dir);  // 1 = down (next page), -1 = up (prev page)

// Async fetch — called from Core 0 worker
bool newsFetch(bool wifiOk, const char *city);
extern bool g_newsPending;
void triggerNewsFetch();

// Accessor for SITREP
const char* screenNewsGetFirstTitle();
