#include "screen_solar.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define SOLAR_CACHE_MS (15UL * 60UL * 1000UL)

struct SolarState {
    bool valid = false;
    bool fetchedOnce = false;
    unsigned long fetchedMs = 0;
    float kp = 0;
    float kpHist[8] = {0};
    int histCount = 0;
    float windKms = 0;
    float density = 0;
    float bz = 0;
    float bt = 0;
    float solarTemp = 0;
    float xrayFlux = 0;
    float cmeSpeedKms = 0;
    char xrayClass[8] = "N/A";
    char flareClass[8] = "NONE";
    char flareTime[14] = "---";
    char cmeTime[14] = "NONE";
    char sync[10] = "--:--";
};

static SolarState s_solar;
static bool s_forceRefresh = false;
bool g_solarPending = false;

static const char *kpCondition(float kp) {
    if (kp < 3) return "QUIET";
    if (kp < 4) return "UNSETTLED";
    if (kp < 5) return "ACTIVE";
    if (kp < 6) return "G1 MINOR";
    if (kp < 7) return "G2 MODERATE";
    if (kp < 8) return "G3 STRONG";
    if (kp < 9) return "G4 SEVERE";
    return "G5 EXTREME";
}

static uint16_t kpColor(float kp) {
    if (kp >= 5) return COL_RED;
    if (kp >= 3) return COL_AMBER;
    return g_themeColor;
}

static const char *auroraLabel(float kp) {
    if (kp < 5) return "NONE";
    if (kp < 7) return "HIGH LAT";
    if (kp < 8) return "POSSIBLE";
    return "LIKELY";
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_solar.sync, sizeof(s_solar.sync), "%s", t);
}

static bool stale() {
    if (!s_solar.fetchedOnce) return true;
    if (s_forceRefresh) return true;
    return millis() - s_solar.fetchedMs > SOLAR_CACHE_MS;
}

static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = 0;
}

void solarFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) {
        return;
    }

    // One request to the Worker replaces five SWPC calls (two of them Range
    // reads against multi-megabyte files) plus the DONKI CME lookup.
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/solar");
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); return; }

    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, ss.c_str())) return;

    SolarState old = s_solar;                 // keep the screen alive if the
    float kp = doc["kp"] | 0.0f;              // payload has no Kp
    if (kp <= 0.0f) { s_solar = old; return; }

    s_solar.kp       = kp;
    s_solar.windKms  = doc["wind"] | 0.0f;
    s_solar.density  = doc["dens"] | 0.0f;
    s_solar.solarTemp= doc["temp"] | 0.0f;
    s_solar.bz       = doc["bz"]   | 0.0f;
    s_solar.bt       = doc["bt"]   | 0.0f;
    s_solar.xrayFlux = doc["xray"] | 0.0f;
    s_solar.cmeSpeedKms = doc["cme"] | 0.0f;

    copyFit(doc["xclass"]    | "N/A", s_solar.xrayClass, sizeof(s_solar.xrayClass));
    copyFit(doc["flare"]     | "NONE", s_solar.flareClass, sizeof(s_solar.flareClass));
    copyFit(doc["flareTime"] | "---", s_solar.flareTime, sizeof(s_solar.flareTime));
    copyFit(doc["cmeTime"]   | "NONE", s_solar.cmeTime, sizeof(s_solar.cmeTime));

    int n = 0;
    for (JsonVariant v : doc["hist"].as<JsonArray>()) {
        if (n >= 8) break;
        s_solar.kpHist[n++] = v | 0.0f;
    }
    s_solar.histCount = n;
    for (int i = n; i < 8; ++i) s_solar.kpHist[i] = s_solar.kp;

    s_solar.valid = true;
    s_solar.fetchedMs = millis();
    s_solar.fetchedOnce = true;
    stampSync();
}

static void drawSolarContent(TFT_eSPI &tft) {
    uint16_t kc = kpColor(s_solar.kp);
    char buf[32];

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, CONTENT_Y + 6);
    tft.print("Kp INDEX");

    snprintf(buf, sizeof(buf), "%.1f", s_solar.kp);
    tft.setTextFont(FONT_NUM);
    tft.setTextColor(kc, COL_BG);
    tft.setCursor(8, CONTENT_Y + 18);
    tft.print(buf);

    tft.setTextFont(FONT_MD);
    tft.setTextColor(kc, COL_BG);
    tft.setCursor(8, CONTENT_Y + 74);
    tft.print(kpCondition(s_solar.kp));

    tft.drawFastVLine(112, CONTENT_Y + 4, 82, g_themeColor);

    struct Stat { const char *label; char value[18]; uint16_t color; } stats[6];
    snprintf(stats[0].value, sizeof(stats[0].value), "%.0f km/s", s_solar.windKms);
    stats[0].label = "WIND"; stats[0].color = COL_WHITE;
    snprintf(stats[1].value, sizeof(stats[1].value), "%.1f nT", s_solar.bz);
    stats[1].label = "Bz"; stats[1].color = COL_WHITE;
    snprintf(stats[2].value, sizeof(stats[2].value), "%.1f p/cc", s_solar.density);
    stats[2].label = "DENS"; stats[2].color = COL_WHITE;
    snprintf(stats[3].value, sizeof(stats[3].value), "%s", s_solar.xrayClass);
    stats[3].label = "XRAY"; stats[3].color = COL_AMBER;
    snprintf(stats[4].value, sizeof(stats[4].value), "%s", s_solar.flareClass);
    stats[4].label = "FLR"; stats[4].color = COL_AMBER;
    if (s_solar.cmeSpeedKms > 0) snprintf(stats[5].value, sizeof(stats[5].value), "%.0f", s_solar.cmeSpeedKms);
    else snprintf(stats[5].value, sizeof(stats[5].value), "%s", s_solar.cmeTime);
    stats[5].label = "CME kms"; stats[5].color = (s_solar.cmeSpeedKms > 0 || strcmp(s_solar.cmeTime, "NONE") != 0) ? COL_RED : COL_WHITE;

    for (int i = 0; i < 6; ++i) {
        int col = i / 3;
        int row = i % 3;
        int x = col == 0 ? 124 : 196;
        int y = CONTENT_Y + 8 + row * 25;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(x, y);
        tft.print(stats[i].label);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(stats[i].color, COL_BG);
        tft.setCursor(x, y + 9);
        tft.print(stats[i].value);
    }

    // 3rd column — BT, TEMP, AURORA (labels FONT_SM, values FONT_MD)
    int ax = 252, ay = CONTENT_Y + 8;

    // BT — row 0
    char btBuf[12];
    snprintf(btBuf, sizeof(btBuf), "%.1f nT", s_solar.bt);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay);
    tft.print("BT");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(ax, ay + 9);
    tft.print(btBuf);

    // TEMP — row 1
    char tempBuf[12];
    if (s_solar.solarTemp >= 1000000)
        snprintf(tempBuf, sizeof(tempBuf), "%.1fM K", s_solar.solarTemp / 1000000.0f);
    else if (s_solar.solarTemp >= 1000)
        snprintf(tempBuf, sizeof(tempBuf), "%.0fK K", s_solar.solarTemp / 1000.0f);
    else if (s_solar.solarTemp > 0)
        snprintf(tempBuf, sizeof(tempBuf), "%.0f K", s_solar.solarTemp);
    else
        snprintf(tempBuf, sizeof(tempBuf), "N/A");
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay + 25);
    tft.print("TEMP");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(ax, ay + 25 + 9);
    tft.print(tempBuf);

    // AURORA — row 2 (bottom)
    const char *al = auroraLabel(s_solar.kp);
    uint16_t ac = COL_WHITE;
    if (s_solar.kp < 5)       ac = COL_WHITE;
    else if (s_solar.kp < 7)  ac = COL_AMBER;
    else if (s_solar.kp < 8)  ac = COL_WHITE;
    else                      ac = COL_RED;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay + 50);
    tft.print("AURORA");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(ac, COL_BG);
    tft.setCursor(ax, ay + 50 + 9);
    tft.print(al);

    int chartY = CONTENT_Y + 104;
    tft.drawFastHLine(0, chartY - 8, SCREEN_W, g_themeColor);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, chartY - 5);
    tft.print("24H Kp HISTORY");

    const int barW = 30;
    const int gap = 8;
    const int maxH = 56;
    const int startX = (SCREEN_W - (8 * barW + 7 * gap)) / 2;
    const int baseY = chartY + 68;
    for (int i = 0; i < 8; ++i) {
        float kp = s_solar.kpHist[i];
        int h = (int)(kp / 9.0f * maxH);
        if (h < 2) h = 2;
        int x = startX + i * (barW + gap);
        uint16_t c = kpColor(kp);
        tft.drawRect(x, baseY - maxH, barW, maxH, COL_DIM);
        tft.fillRect(x + 1, baseY - h, barW - 2, h, c);
        snprintf(buf, sizeof(buf), "%.0f", kp);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(c, COL_BG);
        int tw = tft.textWidth(buf);
        tft.setCursor(x + (barW - tw) / 2, baseY + 4);
        tft.print(buf);
    }
}

void screenSolarDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SOLAR", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 3, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_solar.fetchedOnce || s_forceRefresh || stale()) && wifiOk && !g_solarPending;
    s_forceRefresh = false;

    if (doFetch) {
        triggerSolarFetch();
    }

    if (s_solar.valid) {
        drawSolarContent(tft);
    } else if (g_solarPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching solar...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(92, 100);
        tft.print(wifiOk ? "No solar data" : "Solar offline");
        tft.setTextFont(FONT_SM);
        tft.setCursor(58, 124);
        tft.print("Connect WiFi and revisit this screen");
    }
}

void screenSolarTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_solar.fetchedMs = 0;
    }
}
