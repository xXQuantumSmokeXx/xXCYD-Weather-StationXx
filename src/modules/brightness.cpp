#include "brightness.h"
#include "../config/config.h"
#include "../config/nvs_config.h"
#include <Arduino.h>

#define BATT_PIN 34   // GPIO34 — voltage divider (100k/100k) to LiPo

static const uint8_t s_vals[BRI_LEVELS] = { 0, 60, 100, 150, 200, 255 };
static int s_level = 0;

static void applyBri(uint8_t v) { ledcWrite(0, v); }

void brightnessInit() {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    s_level = nvsGetInt("bri_level", 0);
    if (s_level < 0 || s_level >= BRI_LEVELS) s_level = 0;
    applyBri(s_level > 0 ? s_vals[s_level] : 200);
}

void brightnessSetLevel(int n) {
    if (n < 0 || n >= BRI_LEVELS) return;
    s_level = n;
    nvsPutInt("bri_level", n);
    if (n > 0) applyBri(s_vals[n]);
}

int brightnessGetLevel() { return s_level; }

void brightnessAutoUpdate() {
    if (s_level != 0) return;
    int bri = map(analogRead(LDR_PIN), 0, 4095, 60, 255);
    applyBri((uint8_t)bri);
}

int batteryPct() {
    int raw = analogRead(BATT_PIN);
    if (raw < 50) return -1;                       // near-zero = floating/disconnected
    float v = (raw / 4095.0f) * 3.3f * 2.0f;     // ×2 for 100k/100k voltage divider
    int pct = (int)((v - 3.0f) / (4.2f - 3.0f) * 100.0f);  // 3.0V=0%, 4.2V=100%
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
