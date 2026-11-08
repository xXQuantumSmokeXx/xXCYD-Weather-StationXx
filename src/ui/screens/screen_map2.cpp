#include "screen_map2.h"
#include "screen_map.h"          // shared CONUS basemap + projection
#include "../theme.h"
#include "../theme_color.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../config/nvs_config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <math.h>
#include <cstdio>
#include <cstring>

// ── Layout — mirrors screen_map.cpp exactly ────────────────────────────────
// The shared projection puts CONUS at x 0..319, y CONTENT_Y+10..+179.  The
// last 14 px of the content column are the four-cell layer bar.
#define MAP2_Y0     (CONTENT_Y + 10)      // 33
#define MAP2_H      170
#define LEGEND_Y    (MAP2_Y0 + MAP2_H)    // 203
#define LEGEND_H    14
#define LEGEND_CELL (SCREEN_W / 4)        // 80

// The Worker snaps a 12×7 grid to county centroids, so the payload carries
// per-point coordinates.  The map bbox is used only to decide whether the
// home location is inside the sampled region.
#define WX_LON_MIN  (-125.0f)
#define WX_LON_MAX  (-66.0f)
#define WX_LAT_MIN  (24.5f)
#define WX_LAT_MAX  (49.5f)

#define WX_MAX      96
#define WX_CACHE_MS (15UL * 60UL * 1000UL)

struct WxPoint {
    int16_t lat100;     // 1/100 degree
    int16_t lon100;
    int16_t tempF;      // °F, -999 = missing
    int16_t windMph;    // mph, -1 = missing
    int16_t windDir;    // degrees the wind comes FROM, -1 = missing
    int16_t aqi;        // US AQI, -1 = missing
    int16_t rain100;    // last 24 h, hundredths of an inch
};

static WxPoint s_wx[WX_MAX];
static int s_wxCount = 0;
static bool s_wxFetchedOnce = false;
static unsigned long s_wxFetchedMs = 0;
static unsigned long s_wxLastAttempt = 0;
static bool s_wxForce = false;

bool g_wxMapPending = false;

// ── Layer toggles (NVS-backed, all four on by default) ─────────────────────
static bool s_showRain  = true;
static bool s_showTemp  = true;
static bool s_showWind  = true;
static bool s_showSmoke = true;
static bool s_layersLoaded = false;

#define LAYER_RAIN   1
#define LAYER_TEMP   2
#define LAYER_WIND   4
#define LAYER_SMOKE  8
#define LAYER_ALL    (LAYER_RAIN | LAYER_TEMP | LAYER_WIND | LAYER_SMOKE)

static void layersLoad() {
    if (s_layersLoaded) return;
    int v = nvsGetInt("map2_lay", -1);
    if (v < 0) v = LAYER_ALL;
    s_showRain  = (v & LAYER_RAIN)  != 0;
    s_showTemp  = (v & LAYER_TEMP)  != 0;
    s_showWind  = (v & LAYER_WIND)  != 0;
    s_showSmoke = (v & LAYER_SMOKE) != 0;
    s_layersLoaded = true;
}

static void layersSave() {
    nvsPutInt("map2_lay", (s_showRain  ? LAYER_RAIN  : 0) |
                          (s_showTemp  ? LAYER_TEMP  : 0) |
                          (s_showWind  ? LAYER_WIND  : 0) |
                          (s_showSmoke ? LAYER_SMOKE : 0));
}

// ── Semantic colours — fixed, so the layers stay distinct on every theme ───
// Text on a black map needs to stay legible, so the dark ends of the rain and
// AQI scales are lifted from the fill-marker shades.
#define WX_COL_RAIN_L  0x867Fu   // light blue
#define WX_COL_RAIN_M  0x05FFu   // cyan
#define WX_COL_RAIN_H  0x03DFu   // blue
#define WX_COL_T_FRZ   0x867Fu   // < 32°F
#define WX_COL_T_COLD  0x05FFu   // 32-49°F
#define WX_COL_T_MILD  0x07E0u   // 50-64°F
#define WX_COL_T_WARM  0xFFE0u   // 65-79°F
#define WX_COL_T_HOT   0xFD20u   // 80-94°F
#define WX_COL_T_SCOR  0xF800u   // 95°F+
#define WX_COL_W_CALM  0xAD55u   // silver — light wind
#define WX_COL_W_LOW   0xFFFFu
#define WX_COL_W_MED   0xFFE0u
#define WX_COL_W_HIGH  0xFD20u
#define WX_COL_W_GALE  0xF800u
#define WX_COL_AQ_GOOD 0x07E0u
#define WX_COL_AQ_MOD  0xFFE0u
#define WX_COL_AQ_USG  0xFD20u
#define WX_COL_AQ_UNH  0xF800u
#define WX_COL_AQ_VUNH 0xA014u
#define WX_COL_AQ_HAZ  0xA000u

static uint16_t tempColor(int t) {
    if (t < 32) return WX_COL_T_FRZ;
    if (t < 50) return WX_COL_T_COLD;
    if (t < 65) return WX_COL_T_MILD;
    if (t < 80) return WX_COL_T_WARM;
    if (t < 95) return WX_COL_T_HOT;
    return WX_COL_T_SCOR;
}

static uint16_t rainColor(int r) {
    if (r < 25)  return WX_COL_RAIN_L;
    if (r < 100) return WX_COL_RAIN_M;
    return WX_COL_RAIN_H;
}

static uint16_t windColor(int ws) {
    if (ws < 10) return WX_COL_W_CALM;   // below the draw threshold
    if (ws < 25) return WX_COL_W_MED;    // yellow
    if (ws < 40) return WX_COL_W_HIGH;   // orange
    return WX_COL_W_GALE;                // red
}

// Low smoke uses the theme accent so the digits do not get mistaken for
// TEMP's yellow band; the alert bands keep the usual scale.
static uint16_t aqiColor(int a) {
    if (a <= 50)  return WX_COL_AQ_GOOD;
    if (a <= 100) return g_themeColor;
    if (a <= 150) return WX_COL_AQ_USG;
    if (a <= 200) return WX_COL_AQ_UNH;
    if (a <= 300) return WX_COL_AQ_VUNH;
    return WX_COL_AQ_HAZ;
}

// ── 8×8 legend glyphs (bit 7 = leftmost pixel) ─────────────────────────────
static const uint8_t RAIN_GLYPH[8]  = { 0x10, 0x18, 0x3C, 0x7E, 0x7E, 0x3C, 0x18, 0x00 };
static const uint8_t TEMP_GLYPH[8]  = { 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x7E, 0x3C };
static const uint8_t WIND_GLYPH[8]  = { 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x3C, 0x00, 0x00 };
static const uint8_t SMOKE_GLYPH[8] = { 0x00, 0x66, 0x99, 0x00, 0x66, 0x99, 0x00, 0x00 };

static void drawGlyph(TFT_eSPI &tft, const uint8_t *rows, int cx, int cy,
                      uint16_t col) {
    for (int row = 0; row < 8; row++) {
        uint8_t bits = rows[row];
        int c = 0;
        while (c < 8) {
            if (bits & (0x80 >> c)) {
                int start = c;
                while (c < 8 && (bits & (0x80 >> c))) c++;
                tft.fillRect(cx - 4 + start, cy - 4 + row, c - start, 1, col);
            } else {
                c++;
            }
        }
    }
}

// Wind direction is the direction the wind comes FROM; the arrow points
// where it is going (dir + 180).  0° is north/up.
static void drawWindArrow(TFT_eSPI &tft, int cx, int cy, int dir, uint16_t col) {
    float a  = (dir + 180.0f) * (float)M_PI / 180.0f;
    float ux = sinf(a), uy = -cosf(a);
    float px = -uy, py = ux;

    // 3 px shaft plus a filled head — the old hairline read as a scratch.
    int x0 = cx - (int)(ux * 3.0f), y0 = cy - (int)(uy * 3.0f);
    int x1 = cx + (int)(ux * 3.0f), y1 = cy + (int)(uy * 3.0f);
    tft.drawWideLine(x0, y0, x1, y1, 3.0f, col);

    float tipx = cx + ux * 5.5f, tipy = cy + uy * 5.5f;
    float bx = tipx - ux * 3.0f, by = tipy - uy * 3.0f;
    tft.fillTriangle((int)tipx, (int)tipy,
                     (int)(bx + px * 2.5f), (int)(by + py * 2.5f),
                     (int)(bx - px * 2.5f), (int)(by - py * 2.5f), col);
}

// ── Labels ─────────────────────────────────────────────────────────────────
// TEMP is the upper text line; RAIN is right-aligned on the same line at wet
// points.  WIND is an arrow on the lower line (10 mph+ only); SMOKE is the
// AQI number right-aligned on that lower line (above 50 only).  With all four
// layers on, a wet point can overprint its TEMP label — that is deliberate
// for the density check.
static void drawTempLabel(TFT_eSPI &tft, int x, int y, int tempF) {
    if (tempF <= -900) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", tempF);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(tempColor(tempF), COL_BG);
    int tw = tft.textWidth(buf);
    tft.setCursor(x - tw / 2, y - 10);
    tft.print(buf);
}

static void drawWindIcon(TFT_eSPI &tft, int x, int y, int mph, int dir) {
    if (mph < 10 || dir < 0) return;    // calm air is left blank
    drawWindArrow(tft, x, y + 6, dir, windColor(mph));
}

static void drawRainLabel(TFT_eSPI &tft, int x, int y, int rain100) {
    if (rain100 < 5) return;            // under 0.05" is not worth a label
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", rain100 / 100.0f);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(rainColor(rain100), COL_BG);
    int tw = tft.textWidth(buf);
    int lx = x + 13 - tw;
    if (lx + tw > SCREEN_W - 1) lx = SCREEN_W - 1 - tw;
    if (lx < 0) lx = 0;
    tft.setCursor(lx, y - 10);
    tft.print(buf);
}

static void drawSmokeLabel(TFT_eSPI &tft, int x, int y, int aqi) {
    if (aqi <= 50) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", aqi);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(aqiColor(aqi), COL_BG);
    int tw = tft.textWidth(buf);
    int lx = x + 13 - tw;
    if (lx + tw > SCREEN_W - 1) lx = SCREEN_W - 1 - tw;
    if (lx < 0) lx = 0;
    tft.setCursor(lx, y + 2);
    tft.print(buf);
}

// ── Layer bar ───────────────────────────────────────────────────────────────
// Same geometry as MAP: four borderless cells, icon + label + value.  The
// value is the condition at the sampled point nearest home.
static void drawLegendCell(TFT_eSPI &tft, int idx, const char *label,
                           const char *val, bool on, int kind) {
    int x = idx * LEGEND_CELL;
    tft.fillRect(x, LEGEND_Y, LEGEND_CELL, LEGEND_H, COL_BG);

    char buf[16];
    snprintf(buf, sizeof(buf), "%s %s", label, val);
    tft.setTextFont(FONT_SM);
    int total = 8 + 3 + tft.textWidth(buf);
    int bx = x + (LEGEND_CELL - total) / 2;
    int cy = LEGEND_Y + LEGEND_H / 2;

    uint16_t iconCol = !on ? COL_DIM
                    : kind == 0 ? WX_COL_RAIN_M        // RAIN — cyan
                    : kind == 1 ? WX_COL_T_SCOR        // TEMP — red
                    : kind == 2 ? WX_COL_W_MED         // WIND — yellow
                                : g_themeColor;        // SMOKE — theme
    int icx = bx + 4;
    switch (kind) {
        case 0: drawGlyph(tft, RAIN_GLYPH,  icx, cy, iconCol); break;
        case 1: drawGlyph(tft, TEMP_GLYPH,  icx, cy, iconCol); break;
        case 2: drawGlyph(tft, WIND_GLYPH,  icx, cy, iconCol); break;
        default: drawGlyph(tft, SMOKE_GLYPH, icx, cy, iconCol); break;
    }

    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(bx + 8 + 3, LEGEND_Y + (LEGEND_H - 8) / 2);
    tft.print(buf);
}

static void drawLegendFrame(TFT_eSPI &tft) {
    tft.drawFastHLine(0, LEGEND_Y - 1, SCREEN_W, COL_DIM);
    for (int i = 1; i < 4; i++) {
        int x = i * LEGEND_CELL - 1;
        tft.drawFastVLine(x, LEGEND_Y + 2, LEGEND_H - 4, COL_DIM);
    }
}

static void drawStatus(TFT_eSPI &tft, const char *msg) {
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    int tw = tft.textWidth(msg);
    tft.setCursor((SCREEN_W - tw) / 2, MAP2_Y0 + 78);
    tft.print(msg);
}

// ── Fetch ───────────────────────────────────────────────────────────────────
static bool httpGetJson(const char *url, JsonDocument &doc) {
    if (!WiFi.isConnected()) return false;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    return deserializeJson(doc, ss.c_str()) == DeserializationError::Ok;
}

static int16_t to100(float v) {
    return (int16_t)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
}

static bool wxStale() {
    if (s_wxForce) return true;
    if (!s_wxFetchedOnce) return millis() - s_wxLastAttempt > 30000UL;
    if (millis() - s_wxFetchedMs <= WX_CACHE_MS) return false;
    return millis() - s_wxLastAttempt > 60000UL;   // failed refresh: retry in 1 min
}

bool wxMapFetch(bool wifiOk) {
    s_wxLastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) return false;

    JsonDocument doc;
    if (!httpGetJson("http://quantum-meteor.qsmoke.workers.dev/wxmap", doc)) {
        return false;
    }

    int n = doc["n"] | 0;
    if (n <= 0 || n > WX_MAX) return false;

    JsonArray la = doc["la"].as<JsonArray>();
    JsonArray lo = doc["lo"].as<JsonArray>();
    JsonArray t  = doc["t"].as<JsonArray>();
    JsonArray ws = doc["ws"].as<JsonArray>();
    JsonArray wd = doc["wd"].as<JsonArray>();
    JsonArray aq = doc["aq"].as<JsonArray>();
    JsonArray r  = doc["r"].as<JsonArray>();
    if (la.isNull() || lo.isNull() || t.isNull() || ws.isNull() ||
        wd.isNull() || aq.isNull() || r.isNull()) return false;

    if ((int)la.size() < n || (int)lo.size() < n || (int)t.size() < n ||
        (int)ws.size() < n || (int)wd.size() < n || (int)aq.size() < n ||
        (int)r.size() < n) return false;

    for (int i = 0; i < n; i++) {
        s_wx[i].lat100  = to100(la[i] | 0.0f);
        s_wx[i].lon100  = to100(lo[i] | 0.0f);
        s_wx[i].tempF   = (int16_t)(t[i]  | 0);
        s_wx[i].windMph = (int16_t)(ws[i] | 0);
        s_wx[i].windDir = (int16_t)(wd[i] | 0);
        s_wx[i].aqi     = (int16_t)(aq[i] | 0);
        s_wx[i].rain100 = (int16_t)(r[i]  | 0);
    }

    s_wxCount = n;
    s_wxFetchedOnce = true;
    s_wxFetchedMs = millis();
    return true;
}

// Sampled point nearest home, or -1 when home is outside the bbox.
static int homeIdx() {
    if (!g_location.valid || s_wxCount == 0) return -1;
    if (g_location.lon < WX_LON_MIN || g_location.lon > WX_LON_MAX ||
        g_location.lat < WX_LAT_MIN || g_location.lat > WX_LAT_MAX) return -1;

    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < s_wxCount; i++) {
        float dlat = s_wx[i].lat100 / 100.0f - g_location.lat;
        float dlon = s_wx[i].lon100 / 100.0f - g_location.lon;
        float d = dlat * dlat + dlon * dlon;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ── Draw ────────────────────────────────────────────────────────────────────
void screenMap2Draw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "WX MAP", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 13, 15);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    layersLoad();

    if (wifiOk && !g_wxMapPending && wxStale()) {
        if (triggerWxMapFetch()) s_wxForce = false;
    }

    mapDrawBasemap(tft);

    for (int i = 0; i < s_wxCount; i++) {
        int x, y;
        if (!mapProjectIn(s_wx[i].lon100 / 100.0f,
                          s_wx[i].lat100 / 100.0f, x, y)) continue;
        const WxPoint &p = s_wx[i];

        if (s_showTemp)  drawTempLabel(tft, x, y, p.tempF);
        if (s_showWind)  drawWindIcon(tft, x, y, p.windMph, p.windDir);
        if (s_showRain)  drawRainLabel(tft, x, y, p.rain100);
        if (s_showSmoke) drawSmokeLabel(tft, x, y, p.aqi);
    }

    if (g_location.valid) {
        int hx, hy;
        if (mapProjectIn(g_location.lon, g_location.lat, hx, hy))
            mapDrawHome(tft, hx, hy);
    }

    if (s_wxCount == 0) {
        if (g_wxMapPending)  drawStatus(tft, "FETCHING...");
        else if (!wifiOk)    drawStatus(tft, "OFFLINE");
        else                 drawStatus(tft, "NO DATA");
    }

    // ── Layer bar ─────────────────────────────────────────────────────────
    int hi = homeIdx();
    char rBuf[8], tBuf[8], wBuf[8], aBuf[8];
    if (hi < 0) {
        snprintf(rBuf, sizeof(rBuf), "--");
        snprintf(tBuf, sizeof(tBuf), "--");
        snprintf(wBuf, sizeof(wBuf), "--");
        snprintf(aBuf, sizeof(aBuf), "--");
    } else {
        snprintf(rBuf, sizeof(rBuf), "%.1f", s_wx[hi].rain100 / 100.0f);
        if (s_wx[hi].tempF > -900)
            snprintf(tBuf, sizeof(tBuf), "%dF", s_wx[hi].tempF);
        else
            snprintf(tBuf, sizeof(tBuf), "--");
        if (s_wx[hi].windMph >= 0)
            snprintf(wBuf, sizeof(wBuf), "%d", s_wx[hi].windMph);
        else
            snprintf(wBuf, sizeof(wBuf), "--");
        if (s_wx[hi].aqi >= 0)
            snprintf(aBuf, sizeof(aBuf), "%d", s_wx[hi].aqi);
        else
            snprintf(aBuf, sizeof(aBuf), "--");
    }

    drawLegendCell(tft, 0, "RAIN",  rBuf, s_showRain,  0);
    drawLegendCell(tft, 1, "TEMP",  tBuf, s_showTemp,  1);
    drawLegendCell(tft, 2, "WIND",  wBuf, s_showWind,  2);
    drawLegendCell(tft, 3, "SMOKE", aBuf, s_showSmoke, 3);
    drawLegendFrame(tft);
}

// ── Tap ─────────────────────────────────────────────────────────────────────
bool screenMap2IsControlTap(int16_t y) {
    return y < TOPBAR_H ||
           (y >= LEGEND_Y - 4 && y < LEGEND_Y + LEGEND_H + 4);
}

bool screenMap2Tap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    (void)tft;
    layersLoad();

    // Top bar: force a refresh on the next draw.
    if (y < TOPBAR_H && wifiOk) {
        s_wxForce = true;
        return true;
    }

    if (y >= LEGEND_Y - 4 && y < LEGEND_Y + LEGEND_H + 4) {
        if (x < 0 || x >= SCREEN_W) return false;
        int cell = x / LEGEND_CELL;
        switch (cell) {
            case 0: s_showRain  = !s_showRain;  break;
            case 1: s_showTemp  = !s_showTemp;  break;
            case 2: s_showWind  = !s_showWind;  break;
            default: s_showSmoke = !s_showSmoke; break;
        }
        layersSave();
        return true;
    }
    return false;
}
