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

// ── Simple HTML text extractor — copy text between > and < into buf ────────
static int extractText(const char *html, int start, int end, char *buf, int bufLen) {
    int bi = 0;
    bool inTag = false;
    for (int i = start; i < end && bi < bufLen - 1; i++) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (inTag) continue;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && bi > 0 && buf[bi - 1] == ' ') continue;
        buf[bi++] = c;
    }
    buf[bi] = '\0';
    // Trim trailing spaces
    while (bi > 0 && buf[bi - 1] == ' ') { bi--; buf[bi] = '\0'; }
    // Trim leading spaces
    char *s = buf;
    while (*s == ' ') s++;
    if (s != buf) memmove(buf, s, strlen(s) + 1);
    return bi;
}

// ── Decode common HTML entities in-place ───────────────────────────────────
static void decodeEntities(char *buf) {
    // Simple replacement for &nbsp; — the only entity we really care about
    char *p;
    while ((p = strstr(buf, "&nbsp;")) != nullptr) {
        *p = ' ';
        int rest = strlen(p + 6) + 1;
        memmove(p + 1, p + 6, rest);
    }
    while ((p = strstr(buf, "&amp;")) != nullptr) {
        int rest = strlen(p + 5) + 1;
        memmove(p + 1, p + 5, rest);
        *p = '&';
    }
}

// ── Find next <td>...</td>; return start of content, set *end to > of </td>
static const char *findTd(const char *html, const char *htmlEnd, const char **contentEnd) {
    // Find <td
    while (html < htmlEnd - 3) {
        if (html[0] == '<' && html[1] == 't' && html[2] == 'd') break;
        html++;
    }
    if (html >= htmlEnd - 3) return nullptr;
    // Skip to >
    while (html < htmlEnd && *html != '>') html++;
    if (html >= htmlEnd) return nullptr;
    const char *contentStart = html + 1;  // first char after >
    // Find </td>
    const char *p = contentStart;
    while (p < htmlEnd - 4) {
        if (p[0] == '<' && p[1] == '/' && p[2] == 't' && p[3] == 'd' && p[4] == '>') {
            *contentEnd = p;  // points to < of </td>
            return contentStart;
        }
        p++;
    }
    return nullptr;
}

// ── Parse one <tr>...</tr> from raw HTML ───────────────────────────────────
static bool parseRow(const char *trStart, const char *trEnd, MeteorItem &mi) {
    char buf[256];
    const char *contentEnd;
    const char *td = trStart;

    // TD 0: Event ID — e.g. "&nbsp; Event 5140-2026"
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    extractText(td, contentEnd - td + (contentEnd - td), (int)(contentEnd - td), buf, sizeof(buf));
    decodeEntities(buf);
    const char *evt = strstr(buf, "Event ");
    if (!evt) return false;  // month header row — skip
    evt += 6;
    const char *dash = strchr(evt, '-');
    if (dash && dash > evt && (dash - evt) < 6) {
        int n = dash - evt;
        mi.eventId[0] = '#';
        memcpy(mi.eventId + 1, evt, n);
        mi.eventId[n + 1] = '\0';
    } else {
        copyFit("--", mi.eventId, sizeof(mi.eventId));
    }
    td = contentEnd + 5;  // after </td>

    // TD 1: Report count
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    extractText(td, contentEnd - td + (int)(contentEnd - td), (int)(contentEnd - td), buf, sizeof(buf));
    int reps = atoi(buf);
    if (reps > 0) snprintf(mi.reports, sizeof(mi.reports), "%d reps", reps);
    else copyFit("--", mi.reports, sizeof(mi.reports));
    td = contentEnd + 5;

    // TD 2: UT date "2026-07-07 02:54"
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    extractText(td, contentEnd - td + (int)(contentEnd - td), (int)(contentEnd - td), buf, sizeof(buf));
    if (strlen(buf) >= 10 && buf[4] == '-' && buf[7] == '-') {
        mi.date[0] = buf[5]; mi.date[1] = buf[6];
        mi.date[2] = '-';
        mi.date[3] = buf[8]; mi.date[4] = buf[9];
        mi.date[5] = '\0';
    } else {
        copyFit("--", mi.date, sizeof(mi.date));
    }
    td = contentEnd + 5;

    // TD 3: Local date/time — skip
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    td = contentEnd + 5;

    // TD 4: Countries — "US", "BE FR DE LU NL CH"
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    extractText(td, contentEnd - td + (int)(contentEnd - td), (int)(contentEnd - td), buf, sizeof(buf));
    char countryBuf[64];
    copyFit(buf, countryBuf, sizeof(countryBuf));
    td = contentEnd + 5;

    // TD 5: States — "ID, UT"
    td = findTd(td, trEnd, &contentEnd);
    if (!td) return false;
    extractText(td, contentEnd - td + (int)(contentEnd - td), (int)(contentEnd - td), buf, sizeof(buf));

    // Combine: country + first state
    char locBuf[26];
    int off = 0;
    if (countryBuf[0]) {
        int cl = strlen(countryBuf);
        if (cl > 10) cl = 10;
        memcpy(locBuf, countryBuf, cl);
        off = cl;
    }
    if (buf[0] && off < (int)sizeof(locBuf) - 3) {
        if (off > 0) { locBuf[off++] = ':'; locBuf[off++] = ' '; }
        const char *comma = strchr(buf, ',');
        int take = comma ? (int)(comma - buf) : (int)strlen(buf);
        if (take > 18) take = 18;
        if (off + take >= (int)sizeof(locBuf) - 1) take = (int)sizeof(locBuf) - off - 2;
        memcpy(locBuf + off, buf, take);
        off += take;
        if (comma && off < (int)sizeof(locBuf) - 2) locBuf[off++] = '+';
    }
    locBuf[off] = '\0';
    if (off == 0) copyFit("--", locBuf, sizeof(locBuf));
    copyFit(locBuf, mi.location, sizeof(mi.location));

    return true;
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

    String body = http.getString();
    http.end();
    Serial.printf("[MET] %d bytes\n", body.length());

    // Filter — only parse fields we need, saves RAM
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
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
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
        if (strlen(d) >= 10 && d[4] == '-') {
            mi.date[0] = d[5]; mi.date[1] = d[6];
            mi.date[2] = '-'; mi.date[3] = d[8]; mi.date[4] = d[9];
            mi.date[5] = '\0';
        } else copyFit("--", mi.date, sizeof(mi.date));

        int reps = evt["reports"] | 0;
        if (reps > 0) snprintf(mi.reports, sizeof(mi.reports), "%d reps", reps);
        else copyFit("--", mi.reports, sizeof(mi.reports));

        const char *cc = evt["country"] | "";
        const char *st = evt["state"] | "";
        char loc[26]; int off = 0;
        if (cc[0]) { int cl = strlen(cc); if (cl > 10) cl = 10; memcpy(loc, cc, cl); off = cl; }
        if (st[0] && off < 23) { if (off) { loc[off++] = ':'; loc[off++] = ' '; }
            int sl = strlen(st); if (sl > 18) sl = 18;
            memcpy(loc + off, st, sl); off += sl; }
        loc[off] = '\0';
        if (!off) copyFit("--", loc, sizeof(loc));
        copyFit(loc, mi.location, sizeof(mi.location));

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

// ── Fetch from JPL (fallback — scientific sensor data) ────────────────────
static int fetchJplFireballs() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://ssd-api.jpl.nasa.gov/fireball.api?date-min=2024-01-01&sort=-date&limit=50");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return 0; }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return 0;

    JsonArray fields = doc["fields"].as<JsonArray>();
    JsonArray dataArr = doc["data"].as<JsonArray>();
    if (fields.isNull() || dataArr.isNull()) return 0;

    int idxDate = -1, idxEnergy = -1, idxLat = -1, idxLatDir = -1;
    int idxLon = -1, idxLonDir = -1, idxAlt = -1, idxVel = -1;

    for (int i = 0; i < (int)fields.size(); i++) {
        const char *f = fields[i].as<const char *>();
        if (!f) continue;
        if      (strcmp(f, "date") == 0)     idxDate = i;
        else if (strcmp(f, "energy") == 0)   idxEnergy = i;
        else if (strcmp(f, "lat") == 0)      idxLat = i;
        else if (strcmp(f, "lat-dir") == 0)  idxLatDir = i;
        else if (strcmp(f, "lon") == 0)      idxLon = i;
        else if (strcmp(f, "lon-dir") == 0)  idxLonDir = i;
        else if (strcmp(f, "alt") == 0)      idxAlt = i;
        else if (strcmp(f, "vel") == 0)      idxVel = i;
    }

    int count = 0;
    for (JsonArray row : dataArr) {
        if (count >= METEOR_MAX) break;
        MeteorItem &mi = s_meteors[count];

        const char *d = (idxDate >= 0) ? row[idxDate].as<const char *>() : nullptr;
        if (d && strlen(d) >= 10) {
            mi.date[0] = d[5]; mi.date[1] = d[6];
            mi.date[2] = '-'; mi.date[3] = d[8]; mi.date[4] = d[9];
            mi.date[5] = '\0';
        } else copyFit("--", mi.date, sizeof(mi.date));

        if (idxEnergy >= 0) {
            const char *ej = row[idxEnergy].as<const char *>();
            if (ej && ej[0]) {
                float gj = atof(ej) * 10.0f;
                if (gj >= 1000.0f) snprintf(mi.reports, sizeof(mi.reports), "%.0fGJ", gj);
                else if (gj >= 1.0f) snprintf(mi.reports, sizeof(mi.reports), "%.1fGJ", gj);
                else snprintf(mi.reports, sizeof(mi.reports), "%.2fGJ", gj);
            } else copyFit("--", mi.reports, sizeof(mi.reports));
        } else copyFit("--", mi.reports, sizeof(mi.reports));

        const char *la = (idxLat >= 0) ? row[idxLat].as<const char *>() : nullptr;
        const char *ld = (idxLatDir >= 0) ? row[idxLatDir].as<const char *>() : nullptr;
        const char *lo = (idxLon >= 0) ? row[idxLon].as<const char *>() : nullptr;
        const char *lod = (idxLonDir >= 0) ? row[idxLonDir].as<const char *>() : nullptr;

        if (la && lo) {
            float lat = atof(la), lon = atof(lo);
            if (ld && *ld == 'S') lat = -lat;
            if (lod && *lod == 'W') lon = -lon;
            snprintf(mi.location, sizeof(mi.location), "%.1f%s %.1f%s",
                     fabsf(lat), lat >= 0 ? "N" : "S",
                     fabsf(lon), lon >= 0 ? "E" : "W");
        } else copyFit("--", mi.location, sizeof(mi.location));

        const char *al = (idxAlt >= 0) ? row[idxAlt].as<const char *>() : nullptr;
        const char *ve = (idxVel >= 0) ? row[idxVel].as<const char *>() : nullptr;
        char jplId[10]; int jo = 0;
        if (al && al[0] && jo < 8) jo += snprintf(jplId + jo, sizeof(jplId) - jo, "%.0fkm", atof(al));
        if (ve && ve[0] && jo < 8) {
            if (jo > 0) jplId[jo++] = '/';
            jo += snprintf(jplId + jo, sizeof(jplId) - jo, "%.0fs", atof(ve));
        }
        if (jo > 0) { jplId[jo] = '\0'; copyFit(jplId, mi.eventId, sizeof(mi.eventId)); }
        else copyFit("--", mi.eventId, sizeof(mi.eventId));

        count++;
    }
    return count;
}

// ── Main fetch — IMO first, fall back to JPL ───────────────────────────────
bool meteorsFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    Serial.println("[MET] trying IMO...");
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
    Serial.println("[MET] both sources failed");
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

        if (i & 1)
            tft.fillRect(4, curY, SCREEN_W - 8, ROW_H - 1, COL_INPUTBG);

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

    bool doFetch = (!s_fetchedOnce || s_forceRefresh) && wifiOk && !g_meteorsPending;
    s_forceRefresh = false;

    if (doFetch) {
        g_meteorsPending = true;
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
