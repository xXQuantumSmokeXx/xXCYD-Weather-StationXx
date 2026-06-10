#include "brightness.h"
#include "../config/config.h"
#include "../config/nvs_config.h"
#include "../ui/theme.h"
#include <Arduino.h>

#define BATT_PIN 34   // GPIO34 — voltage divider (100k/100k) to LiPo

static const uint8_t s_vals[BRI_LEVELS] = { 0, 60, 100, 150, 200, 255 };
static int s_level = 0;

// Arduino-ESP32 3.x replaced ledcSetup/ledcAttachPin with ledcAttach,
// and ledcWrite(channel, duty) with ledcWrite(pin, duty).
// Keep backward compatibility so the platform can float on latest.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void applyBri(uint8_t v) { ledcWrite(TFT_BL, v); }
#else
static void applyBri(uint8_t v) { ledcWrite(0, v); }
#endif

void brightnessInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, 5000, 8);
#else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
#endif
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
    if (g_invert) { applyBri(255); return; }
    if (s_level != 0) return;
    int bri = map(analogRead(LDR_PIN), 0, 4095, 60, 255);
    applyBri((uint8_t)bri);
}

void brightnessRestore() {
    if (s_level == 0) {
        int bri = map(analogRead(LDR_PIN), 0, 4095, 60, 255);
        applyBri((uint8_t)bri);
    } else {
        applyBri(s_vals[s_level]);
    }
}

void brightnessOff() {
    applyBri(0);
}

int batteryPct() {
    // Multi-sample averaging to reduce ESP32 ADC noise
    long sum = 0;
    const int samples = 10;
    for (int i = 0; i < samples; i++) {
        sum += analogReadMilliVolts(BATT_PIN);
        delay(2);
    }
    int pinMv = sum / samples;

    if (pinMv < 100) return -1;  // near-zero mV = floating/disconnected

    // analogReadMilliVolts uses factory eFuse calibration — gives actual pin mV
    float vBat = (pinMv / 1000.0f) * 2.0f;  // ×2 for 100k/100k voltage divider

    // Piecewise LiPo discharge curve — linear mapping badly overestimates
    // because LiPo voltage hovers near 3.7-3.8V for most of the discharge.
    int pct;
    if      (vBat >= 4.2f) pct = 100;
    else if (vBat >= 4.1f) pct = (int)(90.0f + (vBat - 4.1f) / 0.1f * 10.0f);
    else if (vBat >= 4.0f) pct = (int)(78.0f + (vBat - 4.0f) / 0.1f * 12.0f);
    else if (vBat >= 3.9f) pct = (int)(63.0f + (vBat - 3.9f) / 0.1f * 15.0f);
    else if (vBat >= 3.8f) pct = (int)(43.0f + (vBat - 3.8f) / 0.1f * 20.0f);
    else if (vBat >= 3.7f) pct = (int)(20.0f + (vBat - 3.7f) / 0.1f * 23.0f);
    else if (vBat >= 3.6f) pct = (int)( 6.0f + (vBat - 3.6f) / 0.1f * 14.0f);
    else if (vBat >= 3.5f) pct = (int)( 1.0f + (vBat - 3.5f) / 0.1f *  5.0f);
    else                   pct = 0;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
