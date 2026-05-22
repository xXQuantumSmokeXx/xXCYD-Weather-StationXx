#include "screenshot.h"

// Screenshot capture is driven by serial commands in main.cpp — the freed
// heap space (no HTTP server) allows a full 8-bit TFT_eSprite (76,800 bytes).

bool screenshotNeedsRedraw() { return false; }
void screenshotInit(TFT_eSPI &, ScreenshotRenderFn) {}
void screenshotLoop() {}
