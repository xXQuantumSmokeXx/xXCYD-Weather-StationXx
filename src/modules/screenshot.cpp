#include "screenshot.h"

// HTTP screenshot server removed — serial capture in main.cpp uses the freed heap
// for a 16-bit TFT_eSprite (153,600 bytes) which requires no WebServer overhead.

bool screenshotNeedsRedraw() { return false; }
void screenshotInit(TFT_eSPI &, ScreenshotRenderFn) {}
void screenshotLoop() {}
