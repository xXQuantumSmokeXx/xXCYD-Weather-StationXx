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
#define FIRE_MAX 24
#define FIRE_ROW_H 14

struct FireItem {
    char title[70];
    char when[8];
};

static FireItem s_fires[FIRE_MAX];
static int s_fireCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[10] = "--:--";
bool g_firesPending = false;

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
    if (s_forceRefresh) return true;
    return millis() - s_fetchedMs > FIRE_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

bool firesFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) {
        return false;
    }

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
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        return false;
    }
    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull()) {
        return false;
    }

    // Collect all events into a temp buffer, sort by date (newest first),
    // then store up to FIRE_MAX for scrollable display.
    const int TEMP_MAX = 32;
    FireItem temp[TEMP_MAX];
    int tempCount = 0;
    for (JsonObject ev : events) {
        if (tempCount >= TEMP_MAX) break;
        const char *title = ev["title"] | "Unknown Fire";
        const char *date = ev["geometry"][0]["date"] | "";
        copyFit(title, temp[tempCount].title, sizeof(temp[tempCount].title));
        if (date && strlen(date) >= 10) snprintf(temp[tempCount].when, sizeof(temp[tempCount].when), "%.5s", date + 5);
        else copyFit("--", temp[tempCount].when, sizeof(temp[tempCount].when));
        tempCount++;
    }

    // Sort by date (newest first)
    for (int i = 0; i < tempCount - 1; i++) {
        for (int j = i + 1; j < tempCount; j++) {
            if (strcmp(temp[i].when, temp[j].when) < 0) {
                FireItem t = temp[i];
                temp[i] = temp[j];
                temp[j] = t;
            }
        }
    }
    int count = tempCount < FIRE_MAX ? tempCount : FIRE_MAX;
    for (int i = 0; i < count; i++) s_fires[i] = temp[i];
    s_fireCount = count;
    s_scrollOff = 0;
    s_fetchedMs = millis();
    s_fetchedOnce = true;
    stampSync();
    return s_fireCount > 0;
}

static void drawFireList(TFT_eSPI &tft) {
    const int HEADER_H = 22;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "OPEN WILDFIRES: %d", s_fireCount);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);
    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / FIRE_ROW_H;
    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_fireCount; i++) {
        if (curY + FIRE_ROW_H > bottomY) break;
        int y = curY;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(4, y + 2);
        tft.print("*");

        int dateW = tft.textWidth(s_fires[i].when);
        int dateX = SCREEN_W - dateW - 4;
        String title = fitText(tft, s_fires[i].title, dateX - 20);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(18, y + 2);
        tft.print(title);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 2);
        tft.print(s_fires[i].when);
        curY += FIRE_ROW_H;
    }

    // Scroll indicators
    if (s_scrollOff + perPage < s_fireCount) {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(SCREEN_W - 20, SCREEN_H - BOTBAR_H - 10);
        tft.print("v");
    }
    if (s_scrollOff > 0) {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(SCREEN_W - 20, CONTENT_Y + HEADER_H);
        tft.print("^");
    }
}

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "FIRES", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 4, 8);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = stale() && wifiOk && !g_firesPending;
    s_forceRefresh = false;

    if (doFetch) {
        g_firesPending = true;
        triggerFiresFetch();
    }

    if (s_fireCount > 0) {
        drawFireList(tft);
    } else if (g_firesPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(86, 108);
        tft.print("Fetching fires...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 58 : 92, 104);
        tft.print(wifiOk ? "No open wildfire data" : "Fires offline");
    }
}

void screenFiresTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenFiresSwipe(int dir) {
    int headerH = 22;
    int perPage = (CONTENT_H - headerH) / FIRE_ROW_H;
    int maxOff = s_fireCount - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}
