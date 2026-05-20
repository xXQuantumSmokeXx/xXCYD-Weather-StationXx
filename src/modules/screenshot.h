#pragma once
#include <TFT_eSPI.h>

using ScreenshotRenderFn = void(*)(TFT_eSPI&);

void screenshotInit(TFT_eSPI &tft, ScreenshotRenderFn renderFn);
void screenshotLoop();
bool screenshotNeedsRedraw();
