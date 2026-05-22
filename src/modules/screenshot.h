#pragma once
#include <TFT_eSPI.h>

using ScreenshotRenderFn = void(*)(TFT_eSPI&);

extern bool g_spriteCapture;

void screenshotInit(TFT_eSPI &tft, ScreenshotRenderFn renderFn);
void screenshotLoop();
bool screenshotNeedsRedraw();
