#pragma once
#include <TFT_eSPI.h>

void screenScannerDraw(TFT_eSPI &tft, bool wifiOk);    // full redraw (navigation, scan done)
void screenScannerAnimate(TFT_eSPI &tft);               // light tick (sweep line only, ~12 fps)
