#include "brightness.h"
#include "../config/config.h"
#include "../config/nvs_config.h"
#include "../ui/theme.h"
#include <Arduino.h>

#define BATT_PIN 34   // GPIO34 — voltage divider (100k/100k) to LiPo

// ── Battery calibration ──────────────────────────────────────────────────
// Override via -D build flags in platformio.ini if your divider differs.
#ifndef BATT_DIVIDER_RATIO
#define BATT_DIVIDER_RATIO  2.0f    // 100k/100k = 2.0 nominal; measure yours
#endif
#ifndef BATT_SAG_OFFSET_MV
#define BATT_SAG_OFFSET_MV  30      // mV to offset WiFi TX load sag
#endif
#define BATT_EMA_ALPHA       0.12f  // EMA smoothing (lower = less jitter)
#define BATT_SAMPLES         50     // ADC averaging samples

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
    // Multi-sample averaging to reduce ESP32 ADC noise (50 samples)
    long sum = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        sum += analogReadMilliVolts(BATT_PIN);
        delay(1);
    }
    int pinMv = sum / BATT_SAMPLES;

    if (pinMv < 100) return -1;  // near-zero mV = floating/disconnected

    // analogReadMilliVolts uses factory eFuse calibration — gives actual pin mV.
    // Apply sag compensation before divider multiplication so the offset scales.
    pinMv += BATT_SAG_OFFSET_MV;

    float vBat = (pinMv / 1000.0f) * BATT_DIVIDER_RATIO;

    // Asymmetric EMA filter — slow decay (discharge is gradual, don't jitter),
    // fast rise (charging / just plugged in — track the real voltage quickly).
    static float s_batteryEma = -1.0f;
    if (s_batteryEma < 0.0f) {
        s_batteryEma = vBat;  // seed on first call
    } else {
        float alpha = BATT_EMA_ALPHA;
        if (vBat > s_batteryEma + 0.08f) {
            // Voltage is climbing — charging or freshly plugged in.
            // Use a much faster alpha so the display climbs in seconds, not minutes.
            alpha = 0.5f;
        }
        s_batteryEma = alpha * vBat + (1.0f - alpha) * s_batteryEma;
    }

    // Piecewise LiPo discharge curve — linear mapping badly overestimates
    // because LiPo voltage hovers near 3.7-3.8V for most of the discharge.
    // Uses EMA-smoothed voltage for stable display readout.
    float v = s_batteryEma;
    int pct;
    if      (v >= 4.2f) pct = 100;
    else if (v >= 4.1f) pct = (int)(90.0f + (v - 4.1f) / 0.1f * 10.0f);
    else if (v >= 4.0f) pct = (int)(78.0f + (v - 4.0f) / 0.1f * 12.0f);
    else if (v >= 3.9f) pct = (int)(63.0f + (v - 3.9f) / 0.1f * 15.0f);
    else if (v >= 3.8f) pct = (int)(43.0f + (v - 3.8f) / 0.1f * 20.0f);
    else if (v >= 3.7f) pct = (int)(20.0f + (v - 3.7f) / 0.1f * 23.0f);
    else if (v >= 3.6f) pct = (int)( 6.0f + (v - 3.6f) / 0.1f * 14.0f);
    else if (v >= 3.5f) pct = (int)( 1.0f + (v - 3.5f) / 0.1f *  5.0f);
    else                pct = 0;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
