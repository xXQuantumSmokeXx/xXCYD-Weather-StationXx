#include "screen_meteors.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>

#define METEOR_CACHE_MS (15UL * 60UL * 1000UL)
#define METEOR_MAX  50
#define ROW_H       12
#define HEADER_H    22

// ── Display struct ─────────────────────────────────────────────────────────
struct MeteorItem {
    char date[6];       // "07-07"
    char reports[8];    // "99 reps"
    char location[26];  // "US: ID, UT"
    char eventId[10];   // "#5140"
};

static MeteorItem s_meteors[METEOR_MAX];
static int  s_meteorCount = 0;
static bool s_fetchedOnce  = false;
static unsigned long s_fetchedMs = 0;
static bool s_forceRefresh = false;
static int  s_scrollOff    = 0;
static char s_sync[10] = "--:--";
bool g_meteorsPending = false;

// ── Helpers ─────────────────────────────────────────────────────────────────
static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static bool stale() {
    if (!s_fetchedOnce) return true;
    if (s_forceRefresh) return true;
    return millis() - s_fetchedMs > METEOR_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

// ── Country & state name lookup ────────────────────────────────────────────
struct CodeName { const char *code; const char *name; };

static const char* lookupCountry(const char *code) {
    // Common IMO fireball-reporting countries. US stays short ("US") since
    // state names carry the location detail. Everything else gets expanded.
    static const CodeName table[] = {
        {"US","US"}, {"CA","Canada"}, {"GB","UK"}, {"DE","Germany"},
        {"FR","France"}, {"BE","Belgium"}, {"NL","Netherlands"},
        {"LU","Luxembourg"}, {"CH","Switzerland"}, {"AT","Austria"},
        {"IT","Italy"}, {"ES","Spain"}, {"PT","Portugal"},
        {"DK","Denmark"}, {"NO","Norway"}, {"SE","Sweden"},
        {"FI","Finland"}, {"PL","Poland"}, {"CZ","Czechia"},
        {"SK","Slovakia"}, {"HU","Hungary"}, {"RO","Romania"},
        {"HR","Croatia"}, {"SI","Slovenia"}, {"IE","Ireland"},
        {"AU","Australia"}, {"NZ","New Zealand"}, {"JP","Japan"},
        {"BR","Brazil"}, {"AR","Argentina"}, {"CL","Chile"},
        {"MX","Mexico"}, {"ZA","S. Africa"}, {"IN","India"},
    };
    for (auto &e : table) if (strcmp(code, e.code) == 0) return e.name;
    return code;  // unknown — keep raw code
}

static const char* lookupState(const char *code) {
    static const CodeName table[] = {
        {"AL","Alabama"},{"AK","Alaska"},{"AZ","Arizona"},{"AR","Arkansas"},
        {"CA","California"},{"CO","Colorado"},{"CT","Connecticut"},
        {"DE","Delaware"},{"FL","Florida"},{"GA","Georgia"},
        {"HI","Hawaii"},{"ID","Idaho"},{"IL","Illinois"},{"IN","Indiana"},
        {"IA","Iowa"},{"KS","Kansas"},{"KY","Kentucky"},{"LA","Louisiana"},
        {"ME","Maine"},{"MD","Maryland"},{"MA","Massachusetts"},
        {"MI","Michigan"},{"MN","Minnesota"},{"MS","Mississippi"},
        {"MO","Missouri"},{"MT","Montana"},{"NE","Nebraska"},
        {"NV","Nevada"},{"NH","New Hamp."},{"NJ","New Jersey"},
        {"NM","New Mexico"},{"NY","New York"},{"NC","N. Carolina"},
        {"ND","N. Dakota"},{"OH","Ohio"},{"OK","Oklahoma"},
        {"OR","Oregon"},{"PA","Pennsylvania"},{"RI","Rhode Island"},
        {"SC","S. Carolina"},{"SD","S. Dakota"},{"TN","Tennessee"},
        {"TX","Texas"},{"UT","Utah"},{"VT","Vermont"},{"VA","Virginia"},
        {"WA","Washington"},{"WV","W. Virginia"},{"WI","Wisconsin"},
        {"WY","Wyoming"},{"DC","DC"},
    };
    for (auto &e : table) if (strcmp(code, e.code) == 0) return e.name;
    return code;
}

// ── Format location string: "US: Idaho, Utah" or "Germany +France" ──────────
static void fmtLocation(const char *country, const char *state, char *out, size_t len) {
    if (!country || !country[0]) { copyFit("--", out, len); return; }

    // Split on comma or space (IMO uses space-separated codes: "AT FR DE IT CH")
    char ccBuf[64]; snprintf(ccBuf, sizeof(ccBuf), "%s", country);
    char *ccTok = strtok(ccBuf, ", ");

    // Build country portion
    char coStr[32] = "";
    int coCount = 0;
    while (ccTok && coCount < 4) {
        // Trim leading space
        while (*ccTok == ' ') ccTok++;
        const char *name = lookupCountry(ccTok);
        if (coCount == 0) {
            snprintf(coStr, sizeof(coStr), "%s", name);
        } else if (coCount == 1) {
            size_t cur = strlen(coStr);
            snprintf(coStr + cur, sizeof(coStr) - cur, " +%s", name);
        }
        // After 2 countries, stop naming them (e.g. "France +Germany")
        coCount++;
        ccTok = strtok(nullptr, ", ");
    }
    if (coCount > 2) {
        size_t cur = strlen(coStr);
        snprintf(coStr + cur, sizeof(coStr) - cur, " +%d", coCount - 2);
    }

    // Build state portion (only meaningful for US; expand codes to names)
    char stStr[32] = "";
    if (state && state[0]) {
        char stBuf[64]; snprintf(stBuf, sizeof(stBuf), "%s", state);
        char *stTok = strtok(stBuf, ", ");
        int stCount = 0;
        while (stTok && stCount < 3) {
            while (*stTok == ' ') stTok++;
            const char *sname = (strcmp(country, "US") == 0 || strstr(country, "US"))
                ? lookupState(stTok) : stTok;
            if (stCount == 0) {
                snprintf(stStr, sizeof(stStr), "%s", sname);
            } else {
                size_t cur = strlen(stStr);
                snprintf(stStr + cur, sizeof(stStr) - cur, ", %s", sname);
            }
            stCount++;
            stTok = strtok(nullptr, ", ");
        }
    }

    // Combine
    if (stStr[0]) {
        snprintf(out, len, "%s: %s", coStr, stStr);
    } else {
        snprintf(out, len, "%s", coStr);
    }
}

// ── Fetch from Quantum-Meteor API (Cloudflare Worker) ─────────────────────
#define QM_URL "http://quantum-meteor.assorted-cardboard.workers.dev/fireballs?limit=30"

static int fetchImoFireballs() {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, QM_URL)) {
        Serial.println("[MET] begin fail");
        return 0;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[MET] HTTP %d\n", code);
        http.end();
        return 0;
    }

    // Stream directly — avoids a 5 KB String allocation that can fail on a
    // fragmented heap after the preceding HTTPS fetches.
    JsonDocument filter;
    filter["events"][0]["id"]       = true;
    filter["events"][0]["date_utc"] = true;
    filter["events"][0]["reports"]  = true;
    filter["events"][0]["country"]  = true;
    filter["events"][0]["state"]    = true;
    filter["events"][0]["d_sound"]  = true;
    filter["events"][0]["c_sound"]  = true;
    filter["events"][0]["frag"]     = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[MET] JSON: %s\n", err.c_str());
        return 0;
    }

    int count = 0;
    JsonArray events = doc["events"].as<JsonArray>();
    for (JsonObject evt : events) {
        if (count >= METEOR_MAX) break;
        MeteorItem &mi = s_meteors[count];

        const char *d = evt["date_utc"] | "";
        // API returns YYYY-MM-DD; also handle DD/MM/YYYY for resilience
        if (strlen(d) >= 10 && d[4] == '-' && d[7] == '-') {
            // YYYY-MM-DD
            mi.date[0] = d[5]; mi.date[1] = d[6];  // MM
            mi.date[2] = '-';
            mi.date[3] = d[8]; mi.date[4] = d[9];  // DD
            mi.date[5] = '\0';
        } else if (strlen(d) >= 10 && d[2] == '/' && d[5] == '/') {
            // DD/MM/YYYY
            mi.date[0] = d[3]; mi.date[1] = d[4];  // MM
            mi.date[2] = '-';
            mi.date[3] = d[0]; mi.date[4] = d[1];  // DD
            mi.date[5] = '\0';
        } else copyFit("--", mi.date, sizeof(mi.date));

        int reps = evt["reports"] | 0;
        if (reps > 0) snprintf(mi.reports, sizeof(mi.reports), "%d reps", reps);
        else copyFit("--", mi.reports, sizeof(mi.reports));

        const char *cc = evt["country"] | "";
        const char *st = evt["state"]   | "";
        fmtLocation(cc, st, mi.location, sizeof(mi.location));

        // Sound/frag tags — compact codes
        bool boom = evt["d_sound"] | false;
        bool csnd = evt["c_sound"] | false;
        bool frag = evt["frag"]    | false;
        char tags[10];
        int ti = 0;
        if (boom) { tags[ti++] = 'B'; }
        if (frag) { if (ti) tags[ti++] = '+'; tags[ti++] = 'F'; }
        if (csnd) { if (ti) tags[ti++] = '+'; tags[ti++] = 'C'; }
        if (ti == 0) { tags[0] = '-'; tags[1] = '-'; ti = 2; }
        tags[ti] = '\0';
        copyFit(tags, mi.eventId, sizeof(mi.eventId));

        count++;
    }
    Serial.printf("[MET] QM parsed %d\n", count);
    return count;
}

// ── Main fetch — Quantum-Meteor Cloudflare Worker ──────────────────────────
bool meteorsFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    Serial.println("[MET] fetching...");
    int count = fetchImoFireballs();
    if (count > 0) {
        s_meteorCount = count;
        s_scrollOff   = 0;
        s_fetchedMs   = millis();
        s_fetchedOnce = true;
        stampSync();
        Serial.printf("[MET] done: %d events\n", count);
        return true;
    }
    Serial.println("[MET] fetch failed");
    return false;
}

// ── Text fit helper ────────────────────────────────────────────────────────
static String fitCol(TFT_eSPI &tft, const char *src, int maxPx) {
    String out(src ? src : "");
    if (tft.textWidth(out) <= maxPx) return out;
    while (out.length() > 1) {
        out.remove(out.length() - 1);
        if (tft.textWidth(out) <= maxPx) return out;
    }
    return String("");
}

// ── Draw ───────────────────────────────────────────────────────────────────
static void drawMeteorList(TFT_eSPI &tft) {
    tft.setTextFont(FONT_SM);

    tft.setTextColor(COL_WHITE, COL_BG);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "FIREBALL EVENTS: %d", s_meteorCount);
    tft.setCursor(8, CONTENT_Y + 4);
    tft.print(hdr);

    tft.setTextColor(COL_WHITE, COL_BG);
    int sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 6, CONTENT_Y + 4);
    tft.print(s_sync);

    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);

    int perPage = (CONTENT_H - HEADER_H) / ROW_H;
    int curY    = CONTENT_Y + HEADER_H;
    int botY    = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_meteorCount; i++) {
        if (curY + ROW_H > botY) break;
        int y = curY + 2;
        MeteorItem &m = s_meteors[i];

        // Date — themed
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(4, y);
        tft.print(m.date);

        // Reports — amber
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.setCursor(38, y);
        tft.print(m.reports);

        // Location — themed
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(90, y);
        tft.print(fitCol(tft, m.location, 120));

        // Tags — color-coded by event type
        uint16_t tagCol = g_themeColor;    // all tags themed
        tft.setTextColor(tagCol, COL_BG);
        int eidW = tft.textWidth(m.eventId);
        tft.setCursor(SCREEN_W - eidW - 6, y);
        tft.print(m.eventId);

        curY += ROW_H;
    }
}

void screenMeteorsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "METEORS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 7, 12);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_fetchedOnce || s_forceRefresh || stale()) && wifiOk && !g_meteorsPending;
    s_forceRefresh = false;

    if (doFetch) {
        triggerMeteorsFetch();
    }

    if (s_meteorCount > 0) {
        drawMeteorList(tft);
    } else if (g_meteorsPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(74, 108);
        tft.print("Fetching fireballs...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 56 : 92, 104);
        tft.print(wifiOk ? "No fireball data available" : "Meteors offline");
    }
}

void screenMeteorsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenMeteorsSwipe(int dir) {
    int perPage = (CONTENT_H - HEADER_H) / ROW_H;
    int maxOff  = s_meteorCount - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}
