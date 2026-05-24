#pragma once
#include <TFT_eSPI.h>

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk);
void screenFiresTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenFiresSwipe(int dir);

// Async fetch — called from Core 0 worker
bool firesFetch(bool wifiOk);
extern bool g_firesPending;
void triggerFiresFetch();

// Accessors for SITREP
int screenFiresGetCount();
const char* screenFiresGetFirstTitle();