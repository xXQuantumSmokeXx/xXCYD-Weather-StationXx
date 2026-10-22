#include "screen_outages.h"
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
#include <cstdint>
#include <cctype>

#define OUT_CACHE_MS (10UL * 60UL * 1000UL)   // matches the Worker's 10 min cache
#define OUT_URL      "http://quantum-meteor.qsmoke.workers.dev/grid"
#define OUT_PTS      30
#define HEADER_H     22

#define X0 6
#define X1 (SCREEN_W - 6)

// ── Data ───────────────────────────────────────────────────────────────────
struct OutSeries {
    int32_t now, ago, peak, delta;
    int8_t  dir;          // -1 falling, 0 steady, 1 rising
    uint8_t n;            // points in pts[]
    int32_t pts[OUT_PTS];
};

struct OutStateRow {
    char    abbr[4];
    int32_t out;
};

static OutSeries  s_nat = {};
static OutSeries  s_loc = {};
static bool       s_hasNat = false;
static bool       s_hasLoc = false;
static char       s_localName[24] = "";
static int32_t    s_incidents = 0;
static int32_t    s_counties  = 0;
static OutStateRow s_states[4];
static int        s_stateCount = 0;
static char       s_source[20] = "";

static unsigned long s_attemptMs = 0;   // last fetch attempt — success or fail
static bool          s_forceRefresh = false;
static char          s_sync[20] = "Updated --:--";
bool g_outagesPending = false;

// ── Helpers ─────────────────────────────────────────────────────────────────
static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static bool stale() {
    if (s_attemptMs == 0) return true;   // never attempted
    if (s_forceRefresh) return true;
    return millis() - s_attemptMs > OUT_CACHE_MS;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "Updated %s", t);
}

static void fmtComma(int32_t n, char *buf, size_t len) {
    if (n < 0) n = 0;
    if (n >= 1000000)     snprintf(buf, len, "%d,%03d,%03d", (int)(n / 1000000), (int)((n / 1000) % 1000), (int)(n % 1000));
    else if (n >= 1000)   snprintf(buf, len, "%d,%03d", (int)(n / 1000), (int)(n % 1000));
    else                  snprintf(buf, len, "%d", (int)n);
}

static void fmtShort(int32_t n, char *buf, size_t len) {
    if (n < 0) n = 0;
    if (n >= 100000)     snprintf(buf, len, "%dk", (int)(n / 1000));
    else if (n >= 1000)  snprintf(buf, len, "%.1fk", n / 1000.0f);
    else                 snprintf(buf, len, "%d", (int)n);
}

static bool readSeries(JsonVariant v, OutSeries &s) {
    if (v.isNull()) return false;
    s.now   = v["now"]   | 0;
    s.ago   = v["ago"]   | 0;
    s.peak  = v["peak"]  | 0;
    s.delta = v["delta"] | 0;
    s.dir   = (int8_t)(v["dir"] | 0);
    s.n     = 0;

    JsonArray pts = v["series"];
    for (JsonVariant p : pts) {
        if (s.n >= OUT_PTS) break;
        s.pts[s.n++] = p.as<long>();
    }
    if (s.peak < s.now) s.peak = s.now;
    return true;
}

// ── Fetch from Quantum-Meteor Worker ────────────────────────────────────────
bool outagesFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) return false;

    Serial.println("[OUT] fetching...");
    s_attemptMs = millis();   // back off failures too

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, OUT_URL)) {
        Serial.println("[OUT] begin fail");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OUT] HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[OUT] JSON: %s\n", err.c_str());
        return false;
    }

    OutSeries nat = {};
    bool hasNat = readSeries(doc["national"], nat);

    OutSeries loc = {};
    bool hasLoc = readSeries(doc["local"], loc);
    char localName[24] = "";
    if (hasLoc) copyFit((const char*)(doc["local"]["name"] | ""), localName, sizeof(localName));

    OutStateRow rows[4];
    int rowCount = 0;
    JsonArray st = doc["states"];
    for (JsonVariant s : st) {
        if (rowCount >= 4) break;
        copyFit((const char*)(s["abbr"] | ""), rows[rowCount].abbr, sizeof(rows[rowCount].abbr));
        rows[rowCount].out = s["out"] | 0;
        rowCount++;
    }

    int32_t incidents = doc["incidents"] | 0;
    int32_t counties  = doc["counties"]  | 0;
    char source[20] = "";
    copyFit((const char*)(doc["source"] | ""), source, sizeof(source));

    if (!hasNat) return false;   // nothing usable — keep the previous data

    s_nat = nat;
    s_hasNat = hasNat;
    s_loc = loc;
    s_hasLoc = hasLoc;
    copyFit(localName, s_localName, sizeof(s_localName));
    s_incidents = incidents;
    s_counties  = counties;
    copyFit(source, s_source, sizeof(s_source));
    s_stateCount = rowCount;
    for (int i = 0; i < rowCount; i++) s_states[i] = rows[i];

    stampSync();
    Serial.printf("[OUT] now=%ld incidents=%ld states=%d local=%d\n",
                  (long)s_nat.now, (long)s_incidents, s_stateCount, s_hasLoc ? 1 : 0);
    return true;
}

// ── Draw helpers ────────────────────────────────────────────────────────────
static void drawDelta(TFT_eSPI &tft, const OutSeries &s, int y) {
    char mag[12], txt[18];
    int32_t m = s.delta < 0 ? -s.delta : s.delta;
    fmtShort(m, mag, sizeof(mag));

    if (s.dir > 0)      snprintf(txt, sizeof(txt), "+%s", mag);
    else if (s.dir < 0) snprintf(txt, sizeof(txt), "-%s", mag);
    else                snprintf(txt, sizeof(txt), "flat");

    uint16_t col = (s.dir > 0) ? COL_AMBER : (s.dir < 0 ? 0x07E0u : g_themeColor);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(col, COL_BG);
    int tw = tft.textWidth(txt);
    int tx = X1 - tw;
    tft.setCursor(tx, y);
    tft.print(txt);

    int cx = tx - 7;
    int cy = y + 4;
    if (s.dir > 0)      tft.fillTriangle(cx, cy + 3, cx + 6, cy + 3, cx + 3, cy - 3, col);
    else if (s.dir < 0) tft.fillTriangle(cx, cy - 3, cx + 6, cy - 3, cx + 3, cy + 3, col);
}

// One 24 h block: label + delta on the top row, bars, then now/peak.
static void drawChart(TFT_eSPI &tft, const OutSeries &s, const char *label, int y, int barH) {
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(X0, y);
    tft.print(label);
    drawDelta(tft, s, y);

    int barTop = y + 12;
    int barBot = barTop + barH;
    tft.drawFastHLine(X0, barBot, SCREEN_W - 2 * X0, g_themeColor);

    if (s.n >= 2) {
        int32_t maxv = s.peak > 0 ? s.peak : 1;
        int areaW = SCREEN_W - 2 * X0;
        int bw = (areaW - (s.n - 1)) / s.n;
        if (bw < 2) bw = 2;

        for (int i = 0; i < s.n; i++) {
            int bh = (int)(((int64_t)s.pts[i] * barH) / maxv);
            if (bh < 1 && s.pts[i] > 0) bh = 1;
            if (bh == 0) continue;
            int x = X0 + i * (bw + 1);
            uint16_t c = (i == s.n - 1) ? COL_WHITE : g_themeColor;
            tft.fillRect(x, barBot - bh, bw, bh, c);
        }
    } else {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(X0, barTop + barH / 2 - 4);
        tft.print("no 24h history");
    }

    char nowBuf[12], peakBuf[12], txt[24];
    fmtShort(s.now, nowBuf, sizeof(nowBuf));
    fmtShort(s.peak, peakBuf, sizeof(peakBuf));

    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(txt, sizeof(txt), "%s now", nowBuf);
    tft.setCursor(X0, barBot + 3);
    tft.print(txt);

    tft.setTextColor(g_themeColor, COL_BG);
    snprintf(txt, sizeof(txt), "peak %s", peakBuf);
    int pw = tft.textWidth(txt);
    tft.setCursor(X1 - pw, barBot + 3);
    tft.print(txt);
}

static void drawOutHeader(TFT_eSPI &tft) {
    char hdr[40], cbuf[16];
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_WHITE, COL_BG);

    if (s_hasNat) {
        fmtComma(s_nat.now, cbuf, sizeof(cbuf));
        snprintf(hdr, sizeof(hdr), "REPORTED OUT: %s", cbuf);
    } else {
        copyFit("REPORTED OUT: --", hdr, sizeof(hdr));
    }
    tft.setCursor(8, CONTENT_Y + 7);
    tft.print(hdr);

    tft.setTextColor(COL_WHITE, COL_BG);
    int sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 6, CONTENT_Y + 7);
    tft.print(s_sync);

    tft.drawFastHLine(0, CONTENT_Y + HEADER_H - 4, SCREEN_W, g_themeColor);
}

static void drawOutTotals(TFT_eSPI &tft, int y) {
    char a[16], b[16], line[40];

    fmtComma(s_incidents, a, sizeof(a));
    fmtComma(s_counties, b, sizeof(b));

    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(line, sizeof(line), "%s incidents", a);
    tft.setCursor(X0, y);
    tft.print(line);

    // Source tag on the right — ODIN's coverage is participating utilities
    // only, so the figure must not read as a national total.
    tft.setTextColor(g_themeColor, COL_BG);
    if (s_counties > 0) snprintf(line, sizeof(line), "%s counties", b);
    else                copyFit(s_source, line, sizeof(line));
    int w = tft.textWidth(line);
    tft.setCursor(X1 - w, y);
    tft.print(line);

    int x = X0;
    tft.setTextColor(g_themeColor, COL_BG);
    if (s_stateCount > 0) {
        for (int i = 0; i < s_stateCount; i++) {
            char o[12], cell[24];
            fmtShort(s_states[i].out, o, sizeof(o));
            snprintf(cell, sizeof(cell), "%s %s", s_states[i].abbr, o);
            tft.setCursor(x, y + 19);
            tft.print(cell);
            x += tft.textWidth(cell) + 10;
        }
    } else {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(X0, y + 21);
        tft.print("source: DOE/ORNL");
    }
}

// ── Screen ─────────────────────────────────────────────────────────────────
void screenOutagesDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "OUTAGES", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 8, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (s_forceRefresh || stale()) && wifiOk && !g_outagesPending;
    s_forceRefresh = false;

    if (doFetch) triggerOutagesFetch();

    if (!s_hasNat) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        const char *msg = g_outagesPending ? "Fetching outages..."
                        : (wifiOk ? "No outage data available" : "Outages offline");
        int w = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - w) / 2, 108);
        tft.print(msg);
        return;
    }

    drawOutHeader(tft);

    if (s_hasLoc) {
        drawChart(tft, s_nat, "NATIONAL 24H", 45, 26);

        char label[32];
        size_t i = 0;
        for (; s_localName[i] && i < sizeof(label) - 6; i++) label[i] = (char)toupper((unsigned char)s_localName[i]);
        label[i] = '\0';
        strncat(label, " 24H", sizeof(label) - strlen(label) - 1);
        drawChart(tft, s_loc, label, 106, 26);
    } else {
        drawChart(tft, s_nat, "NATIONAL 24H", 45, 42);

        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(X0, 124);
        tft.print("Local state unavailable");
    }

    drawDivider(tft, 168, g_themeColor);
    drawOutTotals(tft, 171);
}

void screenOutagesTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    (void)tft; (void)x;
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_attemptMs = 0;   // retry until a fetch actually runs
    }
}

void screenOutagesSwipe(int dir) {
    (void)dir;   // single-glance screen — no scrolling
}
