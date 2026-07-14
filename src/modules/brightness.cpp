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
#define BATT_SAG_OFFSET_MV  100     // mV to offset WiFi+display load sag
#endif
#define BATT_EMA_ALPHA       0.25f  // EMA smoothing (higher = faster response)
#define BATT_HYST            2      // ±2% display hysteresis
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
    // Trimmed-mean sampling — collect 50 samples, sort, discard the
    // bottom and top 10% (5 each) as WiFi TX / load-spike outliers,
    // then average the middle 40 for a stable reading.
    uint16_t raw[BATT_SAMPLES];
    for (int i = 0; i < BATT_SAMPLES; i++) {
        raw[i] = (uint16_t)analogReadMilliVolts(BATT_PIN);
        delay(1);
    }
    // Insertion sort — 50 elements, fast enough on ESP32
    for (int i = 1; i < BATT_SAMPLES; i++) {
        uint16_t key = raw[i];
        int j = i - 1;
        while (j >= 0 && raw[j] > key) { raw[j + 1] = raw[j]; j--; }
        raw[j + 1] = key;
    }
    const int TRIM = BATT_SAMPLES / 10;   // 5 from each end
    long sum = 0;
    for (int i = TRIM; i < BATT_SAMPLES - TRIM; i++) sum += raw[i];
    int pinMv = sum / (BATT_SAMPLES - 2 * TRIM);

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

    // Smooth cubic polynomial fit of real LiPo discharge curve.
    // pct(x) = a*x³ + b*x² + c*x  where x = vBat - 3.5V
    // Fitted to pass through (3.5V,0%), (3.7V,20%), (3.95V,70%), (4.2V,100%).
    // No segment boundaries → no more boundary-jump jitter.
    float v = s_batteryEma;
    int pct;
    if (v >= 4.2f)      pct = 100;
    else if (v <= 3.5f) pct = 0;
    else {
        float x = v - 3.5f;
        float pctFloat = -546.03f * x*x*x + 577.14f * x*x + 6.41f * x;
        pct = (int)(pctFloat + 0.5f);  // round to nearest
    }

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    // Hysteresis — only update displayed % if change > ±2%
    static int s_lastPct = -1;
    if (s_lastPct < 0 || abs(pct - s_lastPct) > BATT_HYST) {
        s_lastPct = pct;
    }
    return s_lastPct;
}
