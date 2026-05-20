#include "wifi_config.h"
#include "../config/config.h"
#include "../config/nvs_config.h"
#include <SD.h>
#include <SPI.h>
#include <cstring>

// wifi.txt format (two lines, no labels):
//   Line 1: SSID
//   Line 2: Password

static void trimCRLF(char *s) {
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' '))
        s[--n] = '\0';
}

void wifiConfigLoad() {
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) return;   // no SD card — use NVS

    File f = SD.open("/wifi.txt");
    if (!f) { SD.end(); return; }   // no wifi.txt — use NVS

    char ssid[64] = {}, pass[64] = {};
    if (f.available()) {
        int n = f.readBytesUntil('\n', ssid, sizeof(ssid) - 1);
        ssid[n] = '\0';
        trimCRLF(ssid);
    }
    if (f.available()) {
        int n = f.readBytesUntil('\n', pass, sizeof(pass) - 1);
        pass[n] = '\0';
        trimCRLF(pass);
    }
    f.close();
    SD.end();

    if (ssid[0]) {
        nvsPutStr("wifi_ssid", ssid);
        nvsPutStr("wifi_pass", pass);
        Serial.println("WiFi credentials loaded from wifi.txt");
    }
}

void wifiGetSSID(char *buf, size_t len) {
    nvsGetStr("wifi_ssid", buf, len, "");
}

void wifiGetPass(char *buf, size_t len) {
    nvsGetStr("wifi_pass", buf, len, "");
}
