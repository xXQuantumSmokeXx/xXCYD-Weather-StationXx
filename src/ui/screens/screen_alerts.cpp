#include "screen_alerts.h"
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

#define ALERT_CACHE_MS (15UL * 60UL * 1000UL)
#define ALERT_MAX 24
#define ALERT_ROW_H 14

struct AlertItem {
    char event[24];
    char headline[96];
    char severity[10];
    char expires[10];
};

static AlertItem s_alerts[ALERT_MAX];
static int s_alertCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[10] = "--:--";
bool g_alertsPending = false;

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
    return millis() - s_fetchedMs > ALERT_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

static uint16_t severityColor(const char *sev) {
    if (strcmp(sev, "Extreme") == 0) return COL_RED;
    if (strcmp(sev, "Severe") == 0)  return COL_AMBER;
    if (strcmp(sev, "Moderate") == 0) return COL_STORM;
    return g_themeColor;
}

// Parse ISO 8601 "2026-05-24T23:00:00+00:00" → local HH:MM
static void parseExpires(const char *iso, char *out, size_t len) {
    if (!iso || strlen(iso) < 16) { copyFit("--:--", out, len); return; }
    int y, mon, d, h, min;
    if (sscanf(iso, "%d-%d-%dT%d:%d", &y, &mon, &d, &h, &min) != 5) {
        copyFit("--:--", out, len); return;
    }
    // Adjust from UTC to local (rough: just add g_location.utcOffset)
    int offsetMin = g_location.valid ? g_location.utcOffset / 60 : 0;
    struct tm ti = {};
    ti.tm_year = y - 1900; ti.tm_mon = mon - 1; ti.tm_mday = d;
    ti.tm_hour = h; ti.tm_min = min; ti.tm_sec = 0;
    time_t tt = mktime(&ti) + g_location.utcOffset;
    struct tm *local = localtime(&tt);
    snprintf(out, len, "%02d:%02d", local->tm_hour, local->tm_min);
}

bool alertsFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.weather.gov/alerts/active?point=%.4f,%.4f",
             g_location.lat, g_location.lon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    http.addHeader("Accept", "application/geo+json");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter["features"][0]["properties"]["event"] = true;
    filter["features"][0]["properties"]["headline"] = true;
    filter["features"][0]["properties"]["severity"] = true;
    filter["features"][0]["properties"]["expires"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        return false;
    }
    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull()) return false;

    const int TEMP_MAX = 40;
    AlertItem temp[TEMP_MAX];
    int tempCount = 0;
    for (JsonObject f : features) {
        if (tempCount >= TEMP_MAX) break;
        JsonObject props = f["properties"];
        if (props.isNull()) continue;

        const char *event    = props["event"] | "";
        const char *headline = props["headline"] | "";
        const char *severity = props["severity"] | "";
        const char *expires  = props["expires"] | "";

        if (!event[0] && !headline[0]) continue;

        copyFit(event,    temp[tempCount].event,    sizeof(temp[tempCount].event));
        copyFit(headline, temp[tempCount].headline, sizeof(temp[tempCount].headline));
        copyFit(severity, temp[tempCount].severity, sizeof(temp[tempCount].severity));
        parseExpires(expires, temp[tempCount].expires, sizeof(temp[tempCount].expires));
        tempCount++;
    }

    int count = tempCount < ALERT_MAX ? tempCount : ALERT_MAX;
    for (int i = 0; i < count; i++) s_alerts[i] = temp[i];
    s_alertCount = count;
    s_fetchedMs = millis();
    s_fetchedOnce = true;
    stampSync();
    return s_alertCount > 0;
}

static void drawAlertList(TFT_eSPI &tft) {
    const int HEADER_H = 22;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "ACTIVE ALERTS: %d", s_alertCount);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);
    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / ALERT_ROW_H;
    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_alertCount; i++) {
        if (curY + ALERT_ROW_H > bottomY) break;
        int y = curY;

        uint16_t sc = severityColor(s_alerts[i].severity);
        tft.setTextFont(FONT_SM);

        // Severity tag (colored, 4-7 chars)
        const char *sevShort = s_alerts[i].severity;
        if (strcmp(sevShort, "Moderate") == 0) sevShort = "Mod";
        else if (strcmp(sevShort, "Unknown") == 0) sevShort = "--";
        int sevW = tft.textWidth(sevShort) + 4;

        tft.setTextColor(sc, COL_BG);
        tft.setCursor(4, y + 2);
        tft.print(sevShort);

        // Event type
        tft.setTextColor(sc, COL_BG);
        tft.setCursor(4 + sevW, y + 2);
        tft.print(s_alerts[i].event);

        int dateW = tft.textWidth(s_alerts[i].expires);
        int dateX = SCREEN_W - dateW - 4;

        // Headline (truncated)
        int headX = 4 + sevW + tft.textWidth(s_alerts[i].event) + 8;
        int headMax = dateX - headX - 4;
        if (headMax > 30) {
            String hl = fitText(tft, s_alerts[i].headline, headMax);
            tft.setTextColor(COL_WHITE, COL_BG);
            tft.setCursor(headX, y + 2);
            tft.print(hl);
        }

        // Expires time
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 2);
        tft.print(s_alerts[i].expires);

        curY += ALERT_ROW_H;
    }
}

void screenAlertsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "ALERTS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 7, 11);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_fetchedOnce || s_forceRefresh) && wifiOk && !g_alertsPending;
    s_forceRefresh = false;

    if (doFetch) {
        g_alertsPending = true;
        triggerAlertsFetch();
    }

    if (s_alertCount > 0) {
        drawAlertList(tft);
    } else if (g_alertsPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(72, 108);
        tft.print("Fetching alerts...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 42 : 68, 104);
        tft.print(wifiOk ? "No active alerts for your area" : "Alerts offline");
        if (wifiOk) {
            tft.setTextFont(FONT_SM);
            tft.setCursor(92, 124);
            tft.print("(no watches or warnings)");
        }
    }
}

void screenAlertsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenAlertsSwipe(int dir) {
    int headerH = 22;
    int perPage = (CONTENT_H - headerH) / ALERT_ROW_H;
    int maxOff = s_alertCount - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}

int screenAlertsGetCount() { return s_alertCount; }

int screenAlertsGetSevereCount() {
    int n = 0;
    for (int i = 0; i < s_alertCount; i++)
        if (strcmp(s_alerts[i].severity, "Extreme") == 0 ||
            strcmp(s_alerts[i].severity, "Severe") == 0) n++;
    return n;
}
