#include "screen_fires.h"
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
#include <ctime>

#define FIRE_CACHE_MS (15UL * 60UL * 1000UL)
#define FIRE_MAX 30
#define FIRE_ROW_H 14
#define MERGE_MAX 80

// ── Raw merge struct (temporary, used during fetch + merge only) ─────────────
struct RawFire {
    char irwinId[40];
    char name[60];
    char state[4];
    double acres;
    char cause[20];
    double pctCont;
    long long discMs;
    bool isLarge;    // >= 10k acres — sorted first
};

// ── Display struct (populated after merge, used by draw) ─────────────────────
struct FireItem {
    char title[70];
    char when[8];
};

static FireItem s_fires[FIRE_MAX];
static int s_fireCount = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static unsigned long s_lastAttempt = 0;   // retry backoff
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[18] = "--:--";
bool g_firesPending = false;

static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

// POOState is "US-CA" format — strip "US-" prefix to just "CA"
static void copyState(const char *src, char *dst, size_t len) {
    if (!src) { dst[0] = '\0'; return; }
    const char *st = src;
    if (strlen(src) >= 5 && src[0] == 'U' && src[1] == 'S' && src[2] == '-')
        st = src + 3;
    copyFit(st, dst, len);
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
    if (!s_fetchedOnce) return millis() - s_lastAttempt > 30000UL;   // retry backoff
    if (s_forceRefresh) return true;
    return millis() - s_fetchedMs > FIRE_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "Updated %s", t);
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

// ── Main fetch — worker task entry point ─────────────────────────────────────
static void formatFireTitle(const RawFire &r, FireItem &fi) {
    // Shorten only the excessively long "Undetermined"
    const char *cs = r.cause;
    if (strcmp(r.cause, "Undetermined") == 0) cs = "Unknown";

    char acBuf[16];
    fmtAcres(r.acres, acBuf, sizeof(acBuf));

    char titleBuf[70];
    int off = snprintf(titleBuf, sizeof(titleBuf), "%s", r.name);
    if (r.state[0] && off < (int)sizeof(titleBuf) - 6)
        off += snprintf(titleBuf + off, sizeof(titleBuf) - off, " [%s]", r.state);
    if (acBuf[0] && off < (int)sizeof(titleBuf) - (int)strlen(acBuf) - 3)
        off += snprintf(titleBuf + off, sizeof(titleBuf) - off, " (%s)", acBuf);
    if (cs[0] && off < (int)sizeof(titleBuf) - 34)
        off += snprintf(titleBuf + off, sizeof(titleBuf) - off,
                        " %s %.0f%% contained", cs, r.pctCont);
    else if (off < (int)sizeof(titleBuf) - 18)
        off += snprintf(titleBuf + off, sizeof(titleBuf) - off,
                        " %.0f%% contained", r.pctCont);
    copyFit(titleBuf, fi.title, sizeof(fi.title));

    // Format discovery date as MM-DD
    int month = 0, day = 0;
    if (r.discMs > 0) {
        time_t tt = (time_t)(r.discMs / 1000LL);
        struct tm *ti = gmtime(&tt);
        if (ti) { month = ti->tm_mon + 1; day = ti->tm_mday; }
    }
    if (month > 0 && day > 0)
        snprintf(fi.when, sizeof(fi.when), "%02d-%02d", month, day);
    else
        copyFit("--", fi.when, sizeof(fi.when));
}

bool firesFetch(bool wifiOk) {
    s_lastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) {
        return false;
    }

    // The Worker runs both ArcGIS queries, merges, dedupes by IrwinID and
    // sorts newest-first; the device gets one finished list in ~5 KB.
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/fire");
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
        if (count >= FIRE_MAX) break;
        RawFire r = {};
        copyFit(it["n"] | "Unknown Fire", r.name, sizeof(r.name));
        copyFit(it["s"] | "", r.state, sizeof(r.state));
        r.acres   = it["ac"] | 0.0;
        copyFit(it["c"] | "", r.cause, sizeof(r.cause));
        r.pctCont = it["pc"] | 0.0;
        r.discMs  = it["t"]  | 0LL;
        formatFireTitle(r, s_fires[count]);
        count++;
    }

    s_fireCount = count;
    s_fetchedOnce = true;
    if (count > 0) {
        s_fetchedMs = millis();
        stampSync();
    } else {
        s_fetchedMs = 0;
    }
    return count > 0;
}

// ── Draw ─────────────────────────────────────────────────────────────────────
static void drawFireList(TFT_eSPI &tft) {
    const int HEADER_H = 22;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_WHITE, COL_BG);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "OPEN WILDFIRES: %d", s_fireCount);
    tft.setCursor(8, CONTENT_Y + 7);
    tft.print(hdr);
    int sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 6, CONTENT_Y + 7);
    tft.print(s_sync);
    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / FIRE_ROW_H;
    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_fireCount; i++) {
        if (curY + FIRE_ROW_H > bottomY) break;
        int y = curY;
        int dateW = tft.textWidth(s_fires[i].when);
        int dateX = SCREEN_W - dateW - 4;
        String title = fitText(tft, s_fires[i].title, dateX - 6);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(4, y + 2);
        tft.print(title);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(dateX, y + 2);
        tft.print(s_fires[i].when);
        curY += FIRE_ROW_H;
    }

}

void screenFiresDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "FIRES", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 6, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (s_forceRefresh || stale()) && wifiOk && !g_firesPending;
    s_forceRefresh = false;

    if (doFetch) {
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
