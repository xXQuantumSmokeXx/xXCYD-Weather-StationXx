#pragma once
#include <cstddef>

// Reads wifi.txt from SD card if present, saves credentials to NVS.
// Call once at boot before tft.init() (SD shares VSPI with touch).
void wifiConfigLoad();

void wifiGetSSID(char *buf, size_t len);
void wifiGetPass(char *buf, size_t len);
