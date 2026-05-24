#pragma once
#include <TFT_eSPI.h>

void screenAlertsDraw(TFT_eSPI &tft, bool wifiOk);
void screenAlertsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk);
void screenAlertsSwipe(int dir);

bool alertsFetch(bool wifiOk);
extern bool g_alertsPending;
void triggerAlertsFetch();

// Accessors for SITREP
int screenAlertsGetCount();
int screenAlertsGetSevereCount();
