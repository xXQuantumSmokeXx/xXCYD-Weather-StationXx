#include "screen_fires.h"
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

#define FIRE_CACHE_MS (15UL * 60UL * 1000UL)
#define FIRE_MAX 10
#define FIRE_ROW_H 16

struct FireItem {
    char title[70];
    char when[8];
};

static FireItem s_fires[FIRE_MAX];
static int s_fireCount = 0;
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

static bool stale() {
    if (!s_fetchedOnce) return true;
    return millis() - s_fetchedMs > FIRE_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

static bool fetchFires(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;
    s_fetchedOnce = true;
    s_fireCount = 0;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://eonet.gsfc.nasa.gov/api/v3/events?category=wildfires&status=open&limit=20");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter["events"][0]["title"] = true;
    filter["events"][0]["geometry"][0]["date"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull()) return false;

    for (JsonObject ev : events) {
        if (s_fireCount >= FIRE_MAX) break;
        const char *title = ev["title"] | "Unknown Fire";
        const char *date = ev["geometry"][0]["date"] | "";
        copyFit(title, s_fires[s_fireCount].title, sizeof(s_fires[s_fireCount].title));
        if (date && strlen(date) >= 10) snprintf(s_fires[s_fireCount].when, sizeof(s_fires[s_fireCount].when), "%.5s", date + 5);
        else copyFit("--", s_fires[s_fireCount].when, sizeof(s_fires[s_fireCount].when));
        s_fireCount++;
    }
    s_fetchedMs = millis();
    stampSync();
    return s_fireCount > 0;
}

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "FIRES", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 4, 7);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (stale() && wifiOk) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(86, 108);
        tft.print("Fetching fires...");
        fetchFires(wifiOk);
        tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);
    }

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "OPEN WILDFIRES: %d", s_fireCount);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);
    tft.drawFastHLine(0, CONTENT_Y + 16, SCREEN_W, g_themeColor);

    if (s_fireCount == 0) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 58 : 92, 104);
        tft.print(wifiOk ? "No open wildfire data" : "Fires offline");
        return;
    }

    int y0 = CONTENT_Y + 22;
    int visible = (CONTENT_H - 24) / FIRE_ROW_H;
    int limit = min(s_fireCount, visible);
    for (int i = 0; i < limit; ++i) {
        int y = y0 + i * FIRE_ROW_H;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(4, y + 3);
        tft.print("*");

        int dateW = tft.textWidth(s_fires[i].when);
        int dateX = SCREEN_W - dateW - 4;
        String title = fitText(tft, s_fires[i].title, dateX - 20);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(18, y + 3);
        tft.print(title);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 3);
        tft.print(s_fires[i].when);
    }
}