#include "screen_solar.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    char sync[10] = "--:--";
};

static SolarState s_solar;

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

static bool readKp(JsonVariant row, float &kp) {
    JsonVariant v;
    if (row.is<JsonArray>()) v = row[1];
    else {
        v = row["Kp"];
        if (v.isNull()) v = row["kp"];
    }
    if (v.isNull()) return false;
    if (v.is<float>() || v.is<int>()) {
        kp = v.as<float>();
        return true;
    }
    const char *s = v.as<const char *>();
    if (!s || !isdigit((unsigned char)s[0])) return false;
    kp = atof(s);
    return true;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_solar.sync, sizeof(s_solar.sync), "%s", t);
}

static bool fetchKp() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray arr = doc.as<JsonArray>();
    float latest[8];
    int count = 0;
    for (int i = (int)arr.size() - 1; i >= 0 && count < 8; --i) {
        float kp = 0;
        if (readKp(arr[i], kp)) latest[count++] = kp;
    }
    if (count == 0) return false;
    s_solar.kp = latest[0];
    s_solar.histCount = count;
    for (int i = 0; i < count; ++i) s_solar.kpHist[i] = latest[count - 1 - i];
    for (int i = count; i < 8; ++i) s_solar.kpHist[i] = s_solar.kp;
    return true;
}

static void fetchPlasma() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/solar-wind/plasma-2-hour.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc.as<JsonArray>();
            int last = arr.size() - 1;
            if (last > 0) {
                if (!arr[last][1].isNull()) s_solar.density = arr[last][1].as<float>();
                if (!arr[last][2].isNull()) s_solar.windKms = arr[last][2].as<float>();
            }
        }
    }
    http.end();
}

static void fetchMag() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/solar-wind/mag-2-hour.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc.as<JsonArray>();
            int last = arr.size() - 1;
            if (last > 0 && !arr[last][3].isNull()) s_solar.bz = arr[last][3].as<float>();
        }
    }
    http.end();
}

static void fetchSolar(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return;
    s_solar.fetchedOnce = true;
    bool kpOk = fetchKp();
    fetchPlasma();
    fetchMag();
    s_solar.valid = kpOk;
    s_solar.fetchedMs = millis();
    stampSync();
}

static bool stale() {
    if (!s_solar.fetchedOnce) return true;
    return millis() - s_solar.fetchedMs > SOLAR_CACHE_MS;
}

void screenSolarDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SOLAR", timeStr, wifiOk);
    drawBottombar(tft, s_solar.valid ? s_solar.sync : "", 3, 7);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (stale() && wifiOk) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching solar...");
        fetchSolar(wifiOk);
        tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);
    }

    if (!s_solar.valid) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(92, 100);
        tft.print(wifiOk ? "No solar data" : "Solar offline");
        tft.setTextFont(FONT_SM);
        tft.setCursor(58, 124);
        tft.print("Connect WiFi and revisit this screen");
        return;
    }

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

    struct Stat { const char *label; char value[18]; uint16_t color; } stats[3];
    snprintf(stats[0].value, sizeof(stats[0].value), "%.0f km/s", s_solar.windKms);
    stats[0].label = "WIND"; stats[0].color = COL_WHITE;
    snprintf(stats[1].value, sizeof(stats[1].value), "%.1f nT", s_solar.bz);
    stats[1].label = "Bz"; stats[1].color = s_solar.bz < -5 ? COL_RED : (s_solar.bz < 0 ? COL_AMBER : g_themeColor);
    snprintf(stats[2].value, sizeof(stats[2].value), "%.1f p/cc", s_solar.density);
    stats[2].label = "DENS"; stats[2].color = COL_WHITE;

    for (int i = 0; i < 3; ++i) {
        int y = CONTENT_Y + 8 + i * 25;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(126, y);
        tft.print(stats[i].label);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(stats[i].color, COL_BG);
        tft.setCursor(126, y + 9);
        tft.print(stats[i].value);
    }

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