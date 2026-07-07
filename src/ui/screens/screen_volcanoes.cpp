#include "screen_volcanoes.h"
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
#include <ctime>

#define VOLCANO_CACHE_MS (15UL * 60UL * 1000UL)
#define VOLCANO_MAX 24
#define VOLCANO_ROW_H 34   // two lines per volcano

struct VolcanoItem {
    char name[40];
    char obs[36];        // observatory full name (e.g. "Yellowstone Volcano Observatory")
    char color_code[8];
    char alert_level[10];
    char when[16];
};

static VolcanoItem s_volcanoes[VOLCANO_MAX];
static int s_volcanoCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[10] = "--:--";
bool g_volcanoesPending = false;

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
    return millis() - s_fetchedMs > VOLCANO_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

// Parse "2026-06-04 19:24:56" → relative time string like "2h ago" or "Jun 4"
static void fmtRelativeTime(const char *utcStr, char *out, size_t len) {
    if (!utcStr || utcStr[0] == '\0' || utcStr[0] == ' ') {
        snprintf(out, len, "--");
        return;
    }
    struct tm ti = {};
    if (sscanf(utcStr, "%d-%d-%d %d:%d:%d",
               &ti.tm_year, &ti.tm_mon, &ti.tm_mday,
               &ti.tm_hour, &ti.tm_min, &ti.tm_sec) < 3) {
        snprintf(out, len, "--");
        return;
    }
    ti.tm_year -= 1900;
    ti.tm_mon  -= 1;
    ti.tm_isdst = -1;
    time_t t = timeIsValid() ? time(nullptr) : 0;

    // Use a simple UTC→epoch conversion (mktime applies local TZ offset, compensate)
    // For embedded, just compute a rough delta
    time_t then = 0;
    {
        // Days since 1970 for the given date (simplified)
        int y = ti.tm_year + 1900;
        int m = ti.tm_mon + 1;
        int d = ti.tm_mday;
        // Zeller-ish: use a simple day-of-year approximation
        static const int daysBefore[13] = {0,0,31,59,90,120,151,181,212,243,273,304,334};
        int doy = daysBefore[m] + d - 1;
        if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) doy++;
        long daysSinceEpoch = (long)(y - 1970) * 365L + (y - 1969) / 4L - (y - 1901) / 100L + (y - 1601) / 400L + doy;
        then = daysSinceEpoch * 86400L + (long)ti.tm_hour * 3600L + (long)ti.tm_min * 60L + ti.tm_sec;
    }

    if (t == 0 || then == 0) {
        // Fallback: show month-day
        snprintf(out, len, "%s %d",
                 (const char*[]){"","Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"}[ti.tm_mon + 1],
                 ti.tm_mday);
        return;
    }

    long diff = (long)(t - then);
    if (diff < 0) diff = 0;
    if (diff < 3600)
        snprintf(out, len, "%ldm ago", diff / 60);
    else if (diff < 86400)
        snprintf(out, len, "%ldh ago", diff / 3600);
    else if (diff < 604800)
        snprintf(out, len, "%ldd ago", diff / 86400);
    else if (diff < 2592000)
        snprintf(out, len, "%ldw ago", diff / 604800);
    else {
        snprintf(out, len, "%s %d",
                 (const char*[]){"","Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"}[ti.tm_mon + 1],
                 ti.tm_mday);
    }
}

// Color code → RGB565 dot color
static uint16_t colorDot(const char *code) {
    if (!code) return COL_DIM;
    if (strcmp(code, "GREEN")  == 0) return 0x07E0;
    if (strcmp(code, "YELLOW") == 0) return COL_AMBER;
    if (strcmp(code, "ORANGE") == 0) return 0xFD20;
    if (strcmp(code, "RED")    == 0) return COL_RED;
    return COL_DIM;
}

bool volcanoesFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://volcanoes.usgs.gov/hans-public/api/volcano/getElevatedVolcanoes");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter[0]["volcano_name"]  = true;
    filter[0]["obs_fullname"]  = true;
    filter[0]["color_code"]    = true;
    filter[0]["alert_level"]   = true;
    filter[0]["sent_utc"]      = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        return false;
    }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) return false;

    const int TEMP_MAX = 40;
    VolcanoItem temp[TEMP_MAX];
    int tempCount = 0;

    // ── Yellowstone always first (hardcoded baseline) ──
    {
        VolcanoItem &y = temp[tempCount++];
        copyFit("Yellowstone", y.name, sizeof(y.name));
        copyFit("Yellowstone Volcano Observatory", y.obs, sizeof(y.obs));
        copyFit("GREEN", y.color_code, sizeof(y.color_code));
        copyFit("NORMAL", y.alert_level, sizeof(y.alert_level));
        copyFit("--", y.when, sizeof(y.when));
    }

    // ── Elevated volcanoes from API ──
    bool yellowstoneElevated = false;
    for (JsonObject v : arr) {
        if (tempCount >= TEMP_MAX) break;

        const char *vname   = v["volcano_name"] | "";
        const char *obs     = v["obs_fullname"] | "";
        const char *color   = v["color_code"]   | "";
        const char *alert   = v["alert_level"]  | "";
        const char *sent    = v["sent_utc"]     | "";

        // Deduplicate: if Yellowstone is in elevated list, replace the hardcoded entry
        if (strcmp(vname, "Yellowstone") == 0) {
            yellowstoneElevated = true;
            copyFit(vname, temp[0].name, sizeof(temp[0].name));
            copyFit(obs,   temp[0].obs,  sizeof(temp[0].obs));
            copyFit(color, temp[0].color_code, sizeof(temp[0].color_code));
            copyFit(alert, temp[0].alert_level, sizeof(temp[0].alert_level));
            char rel[16];
            fmtRelativeTime(sent, rel, sizeof(rel));
            copyFit(rel, temp[0].when, sizeof(temp[0].when));
            continue;
        }

        VolcanoItem &vi = temp[tempCount++];
        copyFit(vname, vi.name, sizeof(vi.name));
        copyFit(obs,   vi.obs,  sizeof(vi.obs));
        copyFit(color, vi.color_code, sizeof(vi.color_code));
        copyFit(alert, vi.alert_level, sizeof(vi.alert_level));
        char rel[16];
        fmtRelativeTime(sent, rel, sizeof(rel));
        copyFit(rel, vi.when, sizeof(vi.when));
    }

    // If Yellowstone NOT elevated, keep it at first position with NORMAL/GREEN
    // (already done above — only replaced if found in elevated list)

    int count = tempCount < VOLCANO_MAX ? tempCount : VOLCANO_MAX;
    for (int i = 0; i < count; i++) s_volcanoes[i] = temp[i];
    s_volcanoCount = count;
    s_fetchedMs = millis();
    s_fetchedOnce = true;
    stampSync();
    return s_volcanoCount > 0;
}

static void drawVolcanoList(TFT_eSPI &tft) {
    const int HEADER_H = 24;
    // Header — small font like other data screens
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_WHITE, COL_BG);
    char hdr[48];
    int elevated = s_volcanoCount - 1; // exclude Yellowstone from elevated count
    if (elevated < 0) elevated = 0;
    snprintf(hdr, sizeof(hdr), "TRACKED:1 + ELEVATED:%d", elevated);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);
    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / VOLCANO_ROW_H;
    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_volcanoCount; i++) {
        if (curY + VOLCANO_ROW_H > bottomY) break;
        int y = curY;

        // Color dot — centered vertically in the two-line block
        uint16_t dotCol = colorDot(s_volcanoes[i].color_code);
        tft.fillCircle(9, y + VOLCANO_ROW_H / 2, 4, dotCol);

        // ── Line 1: Volcano name ──
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(18, y);
        String nameFit = fitText(tft, s_volcanoes[i].name, SCREEN_W - 24);
        tft.print(nameFit);

        // ── Line 2: Observatory / Alert level / Timestamp ──
        tft.setTextFont(FONT_SM);
        int timeW = tft.textWidth(s_volcanoes[i].when);
        int timeX = SCREEN_W - timeW - 4;
        char sub[80];
        snprintf(sub, sizeof(sub), "%s  %s",
                 s_volcanoes[i].obs,
                 s_volcanoes[i].alert_level);
        String subFit = fitText(tft, sub, timeX - 22);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(18, y + 20);
        tft.print(subFit);

        // Timestamp right-aligned on line 2
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(timeX, y + 20);
        tft.print(s_volcanoes[i].when);

        curY += VOLCANO_ROW_H;
    }
}

void screenVolcanoesDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "VOLCANOES", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 8, 12);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_fetchedOnce || s_forceRefresh) && wifiOk && !g_volcanoesPending;
    s_forceRefresh = false;

    if (doFetch) {
        g_volcanoesPending = true;
        triggerVolcanoesFetch();
    }

    if (s_volcanoCount > 0) {
        drawVolcanoList(tft);
    } else if (g_volcanoesPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(78, 108);
        tft.print("Fetching volcanoes...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 68 : 92, 104);
        tft.print(wifiOk ? "No volcano data" : "Volcanoes offline");
    }
}

void screenVolcanoesTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenVolcanoesSwipe(int dir) {
    int headerH = 24;
    int perPage = (CONTENT_H - headerH) / VOLCANO_ROW_H;
    int maxOff = s_volcanoCount - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}
