#include "screen_ews.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

#define EWS_CACHE_MS  (35UL * 60UL * 1000UL)   // 35 min (EWS refreshes every 30)
#define EWS_URL       "http://quantum-meteor.qsmoke.workers.dev/ews"

// ── Display data ─────────────────────────────────────────────────────────────
static int     s_level       = 1;
static char    s_alert[12]   = "normal";
static float   s_zScore      = 0.0f;
static int     s_jets        = 0;
static int     s_baseline    = 0;
static int     s_tracked     = 0;
static char    s_updated[32] = "--";
static bool    s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static bool    s_forceRefresh = false;
static char    s_sync[10]    = "--:--";
bool g_ewsPending = false;

// ── Helpers ─────────────────────────────────────────────────────────────────
static bool stale() {
    if (!s_fetchedOnce) return true;
    if (s_forceRefresh) return true;
    return millis() - s_fetchedMs > EWS_CACHE_MS;
}

static void stampSync() {
    char t[10]; timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

// ── Level color ──────────────────────────────────────────────────────────────
// Green (1) → amber (3) → red (5)
static uint16_t levelColor(int level) {
    switch (level) {
        case 1:  return 0x07E0u;   // green
        case 2:  return 0x9FE0u;   // yellow-green
        case 3:  return COL_AMBER; // amber
        case 4:  return 0xFC00u;   // orange
        case 5:  return COL_RED;   // red
        default: return COL_WHITE;
    }
}

// ── Format comma-separated number into buf ───────────────────────────────────
static void fmtCount(int n, char *buf, size_t len) {
    if (n >= 1000000) {
        snprintf(buf, len, "%d,%03d,%03d", n / 1000000, (n / 1000) % 1000, n % 1000);
    } else if (n >= 1000) {
        snprintf(buf, len, "%d,%03d", n / 1000, n % 1000);
    } else {
        snprintf(buf, len, "%d", n);
    }
}

// ── Fetch from Quantum-Meteor Worker ────────────────────────────────────────
bool ewsFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    Serial.println("[EWS] fetching...");

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, EWS_URL)) {
        Serial.println("[EWS] begin fail");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[EWS] HTTP %d\n", code);
        http.end();
        return false;
    }

    // Lightweight — full response is ~300 bytes, safe for a small JsonDocument
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[EWS] JSON: %s\n", err.c_str());
        return false;
    }

    s_level    = doc["level"]    | 1;
    s_zScore   = doc["z"]        | 0.0f;
    s_jets     = doc["jets"]     | 0;
    s_baseline = doc["baseline"] | 0;
    s_tracked  = doc["tracked"]  | 0;

    const char *alert = doc["alert"] | "normal";
    strncpy(s_alert, alert, sizeof(s_alert) - 1);
    s_alert[sizeof(s_alert) - 1] = '\0';
    // Capitalize first letter for display
    if (s_alert[0] >= 'a' && s_alert[0] <= 'z') s_alert[0] -= 32;

    // Parse ISO-8601 UTC → local time.
    // EWS returns e.g. "2026-07-23T14:59:50+00:00"
    const char *upd = doc["updated"] | "";
    if (upd && strlen(upd) >= 16) {
        int hh = (upd[11] - '0') * 10 + (upd[12] - '0');
        int mm = (upd[14] - '0') * 10 + (upd[15] - '0');
        int offsetHr = g_location.utcOffset / 3600;
        int localHr  = (hh + offsetHr + 24) % 24;
        snprintf(s_updated, sizeof(s_updated), "%02d:%02d local", localHr, mm);
    } else {
        strncpy(s_updated, "--:--", sizeof(s_updated));
    }

    s_fetchedMs   = millis();
    s_fetchedOnce = true;
    stampSync();

    Serial.printf("[EWS] done: level %d, %d jets, z=%.2f\n", s_level, s_jets, s_zScore);
    return true;
}

// ── Draw ─────────────────────────────────────────────────────────────────────
void screenEwsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "AEWS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 5, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_fetchedOnce || s_forceRefresh || stale()) && wifiOk && !g_ewsPending;
    s_forceRefresh = false;

    if (doFetch) {
        triggerEwsFetch();
    }

    if (!s_fetchedOnce && g_ewsPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(62, 108);
        tft.print("Fetching AEWS data...");
        return;
    }

    if (!s_fetchedOnce && !wifiOk) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(98, 104);
        tft.print("AEWS offline");
        return;
    }

    // ── Layout ────────────────────────────────────────────────────────────
    const int cx = SCREEN_W / 2;
    int y = CONTENT_Y + 4;
    uint16_t lc = levelColor(s_level);

    // Subtitle
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    const char *sub = "APOCALYPSE EARLY WARNING SYSTEM";
    int sw = tft.textWidth(sub);
    tft.setCursor(cx - sw / 2, y);
    tft.print(sub);

    // ── Big level number ──────────────────────────────────────────────────
    y += 14;
    tft.setTextFont(FONT_NUM);
    tft.setTextColor(lc, COL_BG);
    char lvl[4]; snprintf(lvl, sizeof(lvl), "%d", s_level);
    int lw = tft.textWidth(lvl);
    tft.setCursor(cx - lw / 2, y);
    tft.print(lvl);

    // "/5" alongside
    tft.setTextFont(FONT_LG);
    tft.setTextColor(lc, COL_BG);
    tft.setCursor(cx + lw / 2 + 6, y + 18);
    tft.print("/5");

    // ── Alert status word ─────────────────────────────────────────────────
    y += 52;
    tft.setTextFont(FONT_LG);
    tft.setTextColor(lc, COL_BG);
    int aw = tft.textWidth(s_alert);
    tft.setCursor(cx - aw / 2, y);
    tft.print(s_alert);

    // ── Z-score ───────────────────────────────────────────────────────────
    y += 28;
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    char zbuf[16];
    snprintf(zbuf, sizeof(zbuf), "%+.2f z", (double)s_zScore);
    int zw = tft.textWidth(zbuf);
    tft.setCursor(cx - zw / 2, y);
    tft.print(zbuf);

    // ── Divider ───────────────────────────────────────────────────────────
    y += 20;
    tft.drawFastHLine(20, y, SCREEN_W - 40, COL_DIM);

    // ── Stats (two rows, two columns) ────────────────────────────────────
    y += 8;
    const int col1x = 16;
    const int col2x = 168;
    tft.setTextFont(FONT_MD);

    auto drawStatRow = [&](int x, const char *label, const char *value, uint16_t vcol) {
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(x, y);
        tft.print(label);
        tft.setTextColor(vcol, COL_BG);
        int lw2 = tft.textWidth(label);
        tft.setCursor(x + lw2 + 6, y);
        tft.print(value);
    };

    char buf[16];

    // Row 1: Jets Airborne | Tracked
    fmtCount(s_jets, buf, sizeof(buf));
    drawStatRow(col1x, "Jets:", buf, COL_WHITE);

    fmtCount(s_tracked, buf, sizeof(buf));
    drawStatRow(col2x, "Tracked:", buf, COL_WHITE);

    y += 20;

    // Row 2: Baseline | Deviation
    fmtCount(s_baseline, buf, sizeof(buf));
    drawStatRow(col1x, "Baseline:", buf, COL_WHITE);

    int dev = s_jets - s_baseline;
    snprintf(buf, sizeof(buf), "%+d", dev);
    drawStatRow(col2x, "Deviation:", buf, (dev > 0) ? COL_AMBER : COL_WHITE);

    // ── Bottom: update info ───────────────────────────────────────────────
    y += 24;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(16, y);
    tft.print("Updated ");
    tft.print(s_updated);
    tft.print("  •  ADS-B Exchange");

    // Sync timestamp right-aligned
    sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 10, y);
    tft.print(s_sync);
}

// ── Tap — force refresh ──────────────────────────────────────────────────────
void screenEwsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
    }
}
