#include "screen_usgs.h"
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
#include <cstdio>
#include <cstring>
#include <time.h>

#define USGS_CACHE_MS (15UL * 60UL * 1000UL)
#define QUAKE_MAX 36
#define QUAKE_ROW_H 14

struct QuakeItem {
    float mag;
    char place[70];
    char when[13];
};

static QuakeItem s_quakes[QUAKE_MAX];
static int s_quakeCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static unsigned long s_lastAttempt = 0;   // retry backoff
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[18] = "--:--";
bool g_usgsPending = false;

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
    if (!s_fetchedOnce) return millis() - s_lastAttempt > 30000UL;   // retry backoff
    if (s_forceRefresh) return true;
    return millis() - s_fetchedMs > USGS_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "Updated %s", t);
}

bool usgsFetch(bool wifiOk) {
    s_lastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) {
        return false;
    }

    // The Quantum-Meteor Worker fetches USGS' CSV and hands back ~2 KB of JSON,
    // so the device never sees the chunked 7.6 KB body or its quoting rules.
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/quakes");
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, ss.c_str())) return false;
    JsonArray arr = doc["items"].as<JsonArray>();
    if (arr.isNull()) return false;

    int count = 0;
    for (JsonObject it : arr) {
        if (count >= QUAKE_MAX) break;
        const char *place = it["p"] | "";
        if (!place[0]) continue;
        s_quakes[count].mag = it["m"] | 0.0f;
        copyFit(place, s_quakes[count].place, sizeof(s_quakes[count].place));
        copyFit(it["w"] | "--", s_quakes[count].when, sizeof(s_quakes[count].when));
        count++;
    }
    if (count == 0) return false;

    s_quakeCount = count;
    s_fetchedOnce = true;
    if (count > 0) {
        s_fetchedMs = millis();
        stampSync();
    } else {
        s_fetchedMs = 0;
    }
    return count > 0;
}

static void drawQuakeList(TFT_eSPI &tft) {
    const int HEADER_H = 22;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_WHITE, COL_BG);
    char hdr[34];
    snprintf(hdr, sizeof(hdr), "M3.5+ QUAKES: %d", s_quakeCount);
    tft.setCursor(8, CONTENT_Y + 7);
    tft.print(hdr);
    int sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 6, CONTENT_Y + 7);
    tft.print(s_sync);
    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / QUAKE_ROW_H;
    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_quakeCount; i++) {
        if (curY + QUAKE_ROW_H > bottomY) break;
        int y = curY;
        char magBuf[8];
        snprintf(magBuf, sizeof(magBuf), "M%.1f", s_quakes[i].mag);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(magColor(s_quakes[i].mag), COL_BG);
        tft.setCursor(4, y + 2);
        tft.print(magBuf);

        int magW = tft.textWidth(magBuf) + 7;
        int dateW = tft.textWidth(s_quakes[i].when);
        int dateX = SCREEN_W - dateW - 4;
        String place = fitText(tft, s_quakes[i].place, dateX - magW - 8);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(magW, y + 2);
        tft.print(place);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 2);
        tft.print(s_quakes[i].when);
        curY += QUAKE_ROW_H;
    }

}

void screenUsgsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "USGS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 7, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (s_forceRefresh || stale()) && wifiOk && !g_usgsPending;
    s_forceRefresh = false;

    if (doFetch) {
        triggerUsgsFetch();
    }

    if (s_quakeCount > 0) {
        drawQuakeList(tft);
    } else if (g_usgsPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching USGS...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 58 : 92, 104);
        tft.print(wifiOk ? "No quake data" : "USGS offline");
    }
}

void screenUsgsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenUsgsSwipe(int dir) {
    int headerH = 22;
    int perPage = (CONTENT_H - headerH) / QUAKE_ROW_H;
    int maxOff = s_quakeCount - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}
