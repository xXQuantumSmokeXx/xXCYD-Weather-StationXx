#pragma once
#include <TFT_eSPI.h>
#include <cstdint>

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk);
void screenFiresTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenFiresSwipe(int dir);

// Async fetch — called from Core 0 worker
bool firesFetch(bool wifiOk);
extern bool g_firesPending;
bool triggerFiresFetch();

// MAP page accessors — read the existing list, no second fetch.
struct FireMapPoint {
    int16_t  lat100;   // latitude  * 100
    int16_t  lon100;   // longitude * 100
    uint16_t acresK;   // acres / 1000, capped
};
int  firesGetMapPoints(FireMapPoint *out, int max);
bool firesMapStale();
void firesMapForceRefresh();
