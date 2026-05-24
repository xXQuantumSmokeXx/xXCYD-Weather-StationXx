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

// Format "1234" → "1,234" for acreage display
static void fmtAcres(double acres, char *out, size_t len) {
    if (acres < 1) { snprintf(out, len, ""); return; }
    long a = (long)acres;
    if (a >= 1000000)
        snprintf(out, len, "%ld,%03ld,%03ld ac", a / 1000000, (a / 1000) % 1000, a % 1000);
    else if (a >= 1000)
        snprintf(out, len, "%ld,%03ld ac", a / 1000, a % 1000);
    else
        snprintf(out, len, "%ld ac", a);
}

bool firesFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) {
        return false;
    }

    // NIFC IMSR — Incident Management Situation Report. Only actual
    // wildfire incidents, no prescribed burns. Updated daily.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services3.arcgis.com/T4QMspbfLg3qTGWY/ArcGIS/rest/"
                       "services/IMSR_Incident_Locations_Most_Recent_View/FeatureServer/0/"
                       "query?where=IsLatest%3D%27x%27+AND+%28x100pct+IS+NULL+OR+x100pct%3C%3E%27c%27%29"
                       "&outFields=fire_name,IrwinFireDiscoveryDateTime,size"
                       "&returnGeometry=false"
                       "&orderByFields=IrwinFireDiscoveryDateTime+DESC"
                       "&resultRecordCount=30&f=json");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter["features"][0]["attributes"]["fire_name"] = true;
    filter["features"][0]["attributes"]["IrwinFireDiscoveryDateTime"] = true;
    filter["features"][0]["attributes"]["size"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        return false;
    }
    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull()) {
        return false;
    }

    const int TEMP_MAX = 32;
    FireItem temp[TEMP_MAX];
    int tempCount = 0;
    for (JsonObject f : features) {
        if (tempCount >= TEMP_MAX) break;
        JsonObject attr = f["attributes"];
        if (attr.isNull()) continue;

        const char *name = attr["fire_name"] | "Unknown Fire";
        const char *dateStr = attr["IrwinFireDiscoveryDateTime"] | "";
        double size = attr["size"] | 0.0;

        // Parse "M/D/YYYY H:MM:SS AM/PM" → "MM-DD"
        int month = 0, day = 0;
        if (dateStr && dateStr[0])
            sscanf(dateStr, "%d/%d/%*d", &month, &day);

        char acBuf[16];
        fmtAcres(size, acBuf, sizeof(acBuf));
        if (acBuf[0])
            snprintf(temp[tempCount].title, sizeof(temp[tempCount].title),
                     "%s (%s)", name, acBuf);
        else
            copyFit(name, temp[tempCount].title, sizeof(temp[tempCount].title));

        if (month > 0 && day > 0)
            snprintf(temp[tempCount].when, sizeof(temp[tempCount].when),
                     "%02d-%02d", month, day);
        else
            copyFit("--", temp[tempCount].when, sizeof(temp[tempCount].when));
        tempCount++;
    }

    // API returns newest-first, but sort as a safeguard
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
