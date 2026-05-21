#include "screen_usgs.h"
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
#include <cstring>
#include <time.h>

#define USGS_CACHE_MS (15UL * 60UL * 1000UL)
#define QUAKE_MAX 10
#define QUAKE_ROW_H 16

struct QuakeItem {
    float mag;
    char place[70];
    char when[12];
};

static QuakeItem s_quakes[QUAKE_MAX];
static int s_quakeCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static char s_sync[10] = "--:--";

static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static String fitText(TFT_eSPI &tft, const char *src, int maxPx) {
    String out(src ? src : "");
    if (tft.textWidth(out) <= maxPx) return out;
    while (out.length() > 2) {
        out.remove(out.length() - 1);
        String c = out + "..";
        if (tft.textWidth(c) <= maxPx) return c;
    }
    return String("..");
}

static uint16_t magColor(float mag) {
    if (mag >= 7.0f) return COL_RED;
    if (mag >= 6.0f) return COL_AMBER;
    if (mag >= 5.0f) return COL_STORM;
    return g_themeColor;
}

static bool stale() {
    if (!s_fetchedOnce) return true;
    return millis() - s_fetchedMs > USGS_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

static void msToWhen(long long ms, char *out, size_t outLen) {
    time_t tt = (time_t)(ms / 1000LL);
    struct tm *ti = gmtime(&tt);
    if (ti) snprintf(out, outLen, "%02d-%02d %02d:%02d", ti->tm_mon + 1, ti->tm_mday, ti->tm_hour, ti->tm_min);
    else copyFit("--", out, outLen);
}

static bool fetchQuakes(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;
    s_fetchedOnce = true;
    s_quakeCount = 0;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://earthquake.usgs.gov/fdsnws/event/1/query?format=geojson&minmagnitude=3.5&limit=20&orderby=time");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter["features"][0]["properties"]["mag"] = true;
    filter["features"][0]["properties"]["place"] = true;
    filter["features"][0]["properties"]["time"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

    for (JsonObject feature : doc["features"].as<JsonArray>()) {
        if (s_quakeCount >= QUAKE_MAX) break;
        JsonObject p = feature["properties"];
        if (p.isNull()) continue;
        s_quakes[s_quakeCount].mag = p["mag"] | 0.0f;
        copyFit(p["place"] | "Unknown", s_quakes[s_quakeCount].place, sizeof(s_quakes[s_quakeCount].place));
        long long ms = p["time"] | 0LL;
        if (ms > 0) msToWhen(ms, s_quakes[s_quakeCount].when, sizeof(s_quakes[s_quakeCount].when));
        else copyFit("--", s_quakes[s_quakeCount].when, sizeof(s_quakes[s_quakeCount].when));
        s_quakeCount++;
    }
    s_fetchedMs = millis();
    stampSync();
    return s_quakeCount > 0;
}

void screenUsgsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "USGS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 5, 7);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (stale() && wifiOk) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching USGS...");
        fetchQuakes(wifiOk);
        tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);
    }

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    char hdr[34];
    snprintf(hdr, sizeof(hdr), "M3.5+ QUAKES: %d", s_quakeCount);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);
    tft.drawFastHLine(0, CONTENT_Y + 16, SCREEN_W, g_themeColor);

    if (s_quakeCount == 0) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 58 : 92, 104);
        tft.print(wifiOk ? "No quake data" : "USGS offline");
        return;
    }

    int y0 = CONTENT_Y + 22;
    int visible = (CONTENT_H - 24) / QUAKE_ROW_H;
    int limit = min(s_quakeCount, visible);
    for (int i = 0; i < limit; ++i) {
        int y = y0 + i * QUAKE_ROW_H;
        char magBuf[8];
        snprintf(magBuf, sizeof(magBuf), "M%.1f", s_quakes[i].mag);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(magColor(s_quakes[i].mag), COL_BG);
        tft.setCursor(4, y + 3);
        tft.print(magBuf);

        int magW = tft.textWidth(magBuf) + 7;
        int dateW = tft.textWidth(s_quakes[i].when);
        int dateX = SCREEN_W - dateW - 4;
        String place = fitText(tft, s_quakes[i].place, dateX - magW - 8);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(magW, y + 3);
        tft.print(place);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 3);
        tft.print(s_quakes[i].when);
    }
}