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

// Field `idx` of a CSV row, quotes stripped.  `place` values look like
// "91 km ESE of Tuensang, India" and carry an embedded comma.
static bool csvField(const char *line, int idx, char *out, size_t len) {
    int  field = 0;
    bool quoted = false;
    size_t o = 0;
    for (const char *p = line; ; ++p) {
        char c = *p;
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if ((c == ',' && !quoted) || c == '\0') {
            if (field == idx) {
                out[o] = '\0';
                return true;
            }
            field++;
            o = 0;
            if (c == '\0') return false;
            continue;
        }
        if (field == idx) {
            if (o + 1 < len) out[o++] = c;
            else                 out[len - 1] = '\0';   // keep truncating safely
        }
    }
}

// "2026-10-23T17:06:25.116Z" -> epoch seconds (UTC).  days-from-civil keeps
// this free of timegm(), which newlib does not reliably provide.
static time_t isoToEpoch(const char *s) {
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    Y -= (M <= 2);
    const int era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400);
    const unsigned doy = (153u * (unsigned)(M + (M > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)D - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + h * 3600 + m * 60 + sec);
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

static void msToWhen(long long ms, char *out, size_t outLen) {
    time_t tt = (time_t)(ms / 1000LL);
    struct tm ti;                       // localtime_r: this runs on the fetch
    if (localtime_r(&tt, &ti)) {        // worker, not just the draw loop
        int h = ti.tm_hour;
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        snprintf(out, outLen, "%02d-%02d %d:%02d%s",
                 ti.tm_mon + 1, ti.tm_mday, h12, ti.tm_min,
                 h >= 12 ? "P" : "A");
    }
    else copyFit("--", out, outLen);
}

bool usgsFetch(bool wifiOk) {
    s_lastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) {
        return false;
    }

    // Fresh client per attempt: a reused TLS session left half-closed after a
    // failure makes every later GET return -1.
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(8);
    HTTPClient http;
    // The screenshot build holds a 76.8 KB framebuffer, leaving too little
    // contiguous heap for the full 40-quake body (~28 KB) — the TLS transfer
    // stalls around 7 KB.  Ask for one page there; release builds keep all 40.
    // CSV rather than GeoJSON: 40 events is ~7.6 KB instead of ~28 KB, which
    // also keeps it under the screenshot build's ~7 KB contiguous-heap limit
    // for TLS bodies.
#ifdef CAPTURE_SPRITE
    const int kLimit = 30;
#else
    const int kLimit = QUAKE_MAX;
#endif
    char url[176];
    snprintf(url, sizeof(url),
             "https://earthquake.usgs.gov/fdsnws/event/1/query?format=csv"
             "&minmagnitude=3.5&limit=%d&orderby=time", kLimit);
    http.begin(client, url);
    // The transfer trickles on this build (large contiguous heap is held by
    // the capture sprite), so the read timeout has to be generous.
    http.setTimeout(45000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    // USGS answers with Transfer-Encoding: chunked.  HTTPClient::getString()
    // reserves (_size + 1), and _size is -1 for chunked, so it falls through
    // and returns an empty string.  writeToStream() decodes the chunk framing.
    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    // Rows are: time,lat,lon,depth,mag,magType,...,place(type is col 14).
    // `place` is quoted and can contain commas, so the splitter honours quotes.
    const int TEMP_MAX = 44;
    QuakeItem temp[TEMP_MAX];
    int tempCount = 0;
    char iso[32], magStr[16], place[sizeof(((QuakeItem *)0)->place)];

    const char *line = ss.c_str();
    bool header = true;
    while (line && *line && tempCount < TEMP_MAX) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > 0 && line[len - 1] == '\r') len--;

        if (header) {
            header = false;                      // first row is the column names
        } else if (len > 0) {
            char row[256];
            size_t n = len < sizeof(row) - 1 ? len : sizeof(row) - 1;
            memcpy(row, line, n);
            row[n] = '\0';

            if (csvField(row, 0, iso, sizeof(iso)) &&
                csvField(row, 4, magStr, sizeof(magStr)) &&
                csvField(row, 13, place, sizeof(place))) {
                QuakeItem &q = temp[tempCount];
                q.mag = atof(magStr);
                copyFit(place[0] ? place : "Unknown", q.place, sizeof(q.place));
                time_t t = isoToEpoch(iso);
                if (t > 0) msToWhen((long long)t * 1000LL, q.when, sizeof(q.when));
                else        copyFit("--", q.when, sizeof(q.when));
                tempCount++;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    if (tempCount == 0) return false;

    // API returns newest-first — no client sort needed
    int count = tempCount < QUAKE_MAX ? tempCount : QUAKE_MAX;
    for (int i = 0; i < count; i++) s_quakes[i] = temp[i];
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
