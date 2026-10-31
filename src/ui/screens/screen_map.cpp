#include "screen_map.h"
#include "../theme.h"
#include "../theme_color.h"
#include "../widgets.h"
#include "../us_map_data.h"
#include "../../config/config.h"
#include "../../config/nvs_config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include "screen_fires.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

// ── Map geometry — must match scripts/gen_us_map.py ─────────────────────────
// The bbox is 59° of longitude by 25° of latitude; 320×170 is its
// equirectangular aspect (59·cos37° / 25 ≈ 1.885).  The cos factor belongs in
// the *aspect*, not in the x scale.  The map owns the full content width; the
// layer buttons float over the ocean corners instead of taking a strip.
#define MAP_LON_MIN  (-125.0f)
#define MAP_LON_MAX   (-66.0f)
#define MAP_LAT_MIN    (24.5f)
#define MAP_LAT_MAX    (49.5f)
#define MAP_X0         0
#define MAP_Y0         (CONTENT_Y + 15)         // breathing room under the top bar
#define MAP_W          320
#define MAP_H          170
#define MAP_X_SCALE    (MAP_W / (MAP_LON_MAX - MAP_LON_MIN))
#define MAP_Y_SCALE    (MAP_H / (MAP_LAT_MAX - MAP_LAT_MIN))

// Must match FIRE_MAX in screen_fires.cpp.
#define FIRE_MAP_MAX   40

// Floating legend buttons in the ocean, auto-sized to icon + text.  Their Y
// is anchored to the content area, not MAP_Y0, so shifting the map leaves
// them where they are.
#define LEGEND_Y        (CONTENT_Y + MAP_H - 11)   // 182
#define LEGEND_H        14
#define LEGEND_PAD      4
#define LEGEND_ICON     8
#define LEGEND_GAP      3
#define LEGEND_MARGIN   4

// ── Map quake data (CONUS, /quakes?us=1) ────────────────────────────────────
#define MAPQ_MAX       60
#define MAPQ_CACHE_MS  (15UL * 60UL * 1000UL)

struct MapQuake {
    int16_t lat100;
    int16_t lon100;
    uint8_t mag10;
};

static MapQuake s_mq[MAPQ_MAX];
static int s_mqCount = 0;
static bool s_mqFetchedOnce = false;
static unsigned long s_mqFetchedMs = 0;
static unsigned long s_mqLastAttempt = 0;
static bool s_mqForce = false;
bool g_mapQuakesPending = false;

// ── Layer toggles (NVS-backed, both on by default) ─────────────────────────
static bool s_showFires = true;
static bool s_showQuakes = true;
static bool s_layersLoaded = false;

// Legend geometry, computed during draw so taps use the same hitboxes.
static int s_fireLegendX = 0, s_fireLegendW = 0;
static int s_quakeLegendX = 0, s_quakeLegendW = 0;

static void layersLoad() {
    if (s_layersLoaded) return;
    int v = nvsGetInt("map_lay", 3);
    s_showFires = (v & 1) != 0;
    s_showQuakes = (v & 2) != 0;
    s_layersLoaded = true;
}

static void layersSave() {
    nvsPutInt("map_lay", (s_showFires ? 1 : 0) | (s_showQuakes ? 2 : 0));
}

// ── Projection ──────────────────────────────────────────────────────────────
static void projectRaw(float lon, float lat, int &x, int &y) {
    int xi = MAP_X0 + (int)((lon - MAP_LON_MIN) * MAP_X_SCALE + 0.5f);
    int yi = MAP_Y0 + (int)((MAP_LAT_MAX - lat) * MAP_Y_SCALE + 0.5f);
    if (xi < MAP_X0) xi = MAP_X0;
    else if (xi >= MAP_X0 + MAP_W) xi = MAP_X0 + MAP_W - 1;
    if (yi < MAP_Y0) yi = MAP_Y0;
    else if (yi >= MAP_Y0 + MAP_H) yi = MAP_Y0 + MAP_H - 1;
    x = xi;
    y = yi;
}

static bool projectIn(float lon, float lat, int &x, int &y) {
    float fx = (lon - MAP_LON_MIN) * MAP_X_SCALE;
    float fy = (MAP_LAT_MAX - lat) * MAP_Y_SCALE;
    if (fx < 0.0f || fx > (float)(MAP_W - 1) ||
        fy < 0.0f || fy > (float)(MAP_H - 1)) return false;
    x = MAP_X0 + (int)(fx + 0.5f);
    y = MAP_Y0 + (int)(fy + 0.5f);
    return true;
}

// ── Basemap ─────────────────────────────────────────────────────────────────
static void drawBasemap(TFT_eSPI &tft) {
    int16_t lon100 = 0;
    bool expectLat = false, havePrev = false;
    int px = 0, py = 0;

    for (size_t i = 0; i < US_MAP_LINES_LEN; i++) {
        int16_t v = (int16_t)pgm_read_word(&US_MAP_LINES[i]);
        if (v == (int16_t)-32768) {      // polyline separator
            expectLat = false;
            havePrev = false;
            continue;
        }
        if (!expectLat) { lon100 = v; expectLat = true; continue; }
        expectLat = false;

        int x, y;
        projectRaw(lon100 / 100.0f, v / 100.0f, x, y);
        if (havePrev) tft.drawLine(px, py, x, y, g_themeColor);
        px = x; py = y; havePrev = true;
    }
}

// ── Icons ───────────────────────────────────────────────────────────────────
// 8×8 flame, drawn as horizontal runs (one fillRect per run, not per pixel).
static const uint8_t FLAME[8] = {
    0x08, 0x18, 0x38, 0x7C, 0x7C, 0x7C, 0x38, 0x00
};

static void drawFlame(TFT_eSPI &tft, int cx, int cy, uint16_t col) {
    for (int row = 0; row < 8; row++) {
        uint8_t bits = FLAME[row];
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

// Semantic marker colors — fixed, not theme-derived, so the layers stay
// distinct on every theme and in e-ink invert.
#define COL_FIRE   0xFD20u   // bright orange
#define COL_QUAKE  0x3FE2u   // neon green

static void drawQuake(TFT_eSPI &tft, int cx, int cy, float mag) {
    int r = mag >= 6.0f ? 6 : mag >= 5.0f ? 5 : mag >= 4.0f ? 4 : 3;
    tft.drawCircle(cx, cy, r, COL_QUAKE);
    if (mag >= 5.0f) tft.drawPixel(cx, cy, COL_QUAKE);
}

static void drawHome(TFT_eSPI &tft, int cx, int cy) {
    uint16_t col = COL_WHITE;
    tft.drawFastHLine(cx - 4, cy, 3, col);
    tft.drawFastHLine(cx + 2, cy, 3, col);
    tft.drawFastVLine(cx, cy - 4, 3, col);
    tft.drawFastVLine(cx, cy + 2, 3, col);
}

// ── Declutter: skip a mark that would sit on one already drawn ─────────────
#define MAX_MARKS 96
static int16_t s_mx[MAX_MARKS];
static int16_t s_my[MAX_MARKS];
static int s_mn = 0;

static bool canPlace(int x, int y, int minD) {
    for (int i = 0; i < s_mn; i++) {
        int dx = x - s_mx[i], dy = y - s_my[i];
        if (dx * dx + dy * dy < minD * minD) return false;
    }
    return true;
}

static void place(int x, int y) {
    if (s_mn < MAX_MARKS) { s_mx[s_mn] = x; s_my[s_mn] = y; s_mn++; }
}

// ── Chips ───────────────────────────────────────────────────────────────────
static int legendWidth(TFT_eSPI &tft, const char *label) {
    tft.setTextFont(FONT_SM);
    return LEGEND_PAD + LEGEND_ICON + LEGEND_GAP + tft.textWidth(label) + LEGEND_PAD;
}

// kind: 0 = fire glyph, 1 = quake ring.  Borderless black legend: the text
// always stays theme-colored; the icon is semantic when on and dim when off.
static void drawLegend(TFT_eSPI &tft, int x, int y, int w,
                       const char *label, bool on, int kind) {
    tft.fillRect(x, y, w, LEGEND_H, COL_BG);

    int icx = x + LEGEND_PAD + LEGEND_ICON / 2;
    int icy = y + LEGEND_H / 2;
    if (kind == 0) drawFlame(tft, icx, icy, on ? COL_FIRE : COL_DIM);
    else           tft.drawCircle(icx, icy, 3, on ? COL_QUAKE : COL_DIM);

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(x + LEGEND_PAD + LEGEND_ICON + LEGEND_GAP,
                  y + (LEGEND_H - 8) / 2);
    tft.print(label);
}

static void drawStatus(TFT_eSPI &tft, const char *msg) {
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    int tw = tft.textWidth(msg);
    tft.setCursor((SCREEN_W - tw) / 2, MAP_Y0 + 78);
    tft.print(msg);
}

// ── Fetch ───────────────────────────────────────────────────────────────────
static bool mapQStale() {
    if (s_mqForce) return true;
    if (!s_mqFetchedOnce) return millis() - s_mqLastAttempt > 30000UL;
    return millis() - s_mqFetchedMs > MAPQ_CACHE_MS;
}

bool mapQuakesFetch(bool wifiOk) {
    s_mqLastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) return false;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/quakes?us=1&min=3.5&days=30");
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
        if (count >= MAPQ_MAX) break;
        if (it["la"].isNull() || it["lo"].isNull()) continue;
        float la = it["la"] | 0.0f;
        float lo = it["lo"] | 0.0f;
        float m  = it["m"]  | 0.0f;
        s_mq[count].lat100 = (int16_t)(la * 100.0f + (la >= 0 ? 0.5f : -0.5f));
        s_mq[count].lon100 = (int16_t)(lo * 100.0f + (lo >= 0 ? 0.5f : -0.5f));
        s_mq[count].mag10  = (uint8_t)(m * 10.0f + 0.5f);
        count++;
    }
    if (count == 0) return false;

    s_mqCount = count;
    s_mqFetchedOnce = true;
    s_mqFetchedMs = millis();
    return true;
}

// ── Draw ────────────────────────────────────────────────────────────────────
struct Mark { int16_t x, y; uint16_t sev; };

static void sortDesc(Mark *m, int n) {
    for (int i = 1; i < n; i++) {
        Mark v = m[i];
        int j = i - 1;
        while (j >= 0 && m[j].sev < v.sev) { m[j + 1] = m[j]; j--; }
        m[j + 1] = v;
    }
}

void screenMapDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "MAP", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 12, 14);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    layersLoad();

    if (wifiOk && !g_firesPending && firesMapStale()) triggerFiresFetch();
    if (wifiOk && !g_mapQuakesPending && mapQStale()) {
        if (triggerMapQuakesFetch()) s_mqForce = false;
    }

    drawBasemap(tft);

    // ── Build projected mark lists ────────────────────────────────────────
    FireMapPoint fp[FIRE_MAP_MAX];
    int fN = firesGetMapPoints(fp, FIRE_MAP_MAX);
    Mark fMarks[FIRE_MAP_MAX];
    int nFire = 0;
    for (int i = 0; i < fN; i++) {
        int x, y;
        if (!projectIn(fp[i].lon100 / 100.0f, fp[i].lat100 / 100.0f, x, y)) continue;
        fMarks[nFire].x = x;
        fMarks[nFire].y = y;
        fMarks[nFire].sev = fp[i].acresK;
        nFire++;
    }
    sortDesc(fMarks, nFire);

    Mark qMarks[MAPQ_MAX];
    int nQuake = 0;
    for (int i = 0; i < s_mqCount; i++) {
        int x, y;
        if (!projectIn(s_mq[i].lon100 / 100.0f, s_mq[i].lat100 / 100.0f, x, y)) continue;
        qMarks[nQuake].x = x;
        qMarks[nQuake].y = y;
        qMarks[nQuake].sev = s_mq[i].mag10;
        nQuake++;
    }
    sortDesc(qMarks, nQuake);

    // ── Marks (quakes first so fires yield to them) ───────────────────────
    s_mn = 0;
    if (s_showQuakes) {
        for (int i = 0; i < nQuake; i++) {
            if (!canPlace(qMarks[i].x, qMarks[i].y, 11)) continue;
            drawQuake(tft, qMarks[i].x, qMarks[i].y, qMarks[i].sev / 10.0f);
            place(qMarks[i].x, qMarks[i].y);
        }
    }
    if (s_showFires) {
        for (int i = 0; i < nFire; i++) {
            if (!canPlace(fMarks[i].x, fMarks[i].y, 7)) continue;
            drawFlame(tft, fMarks[i].x, fMarks[i].y, COL_FIRE);
            place(fMarks[i].x, fMarks[i].y);
        }
    }

    // Home marker last so it stays visible.
    if (g_location.valid) {
        int hx, hy;
        if (projectIn(g_location.lon, g_location.lat, hx, hy)) drawHome(tft, hx, hy);
    }

    // ── Status when the visible map is empty ──────────────────────────────
    int visible = (s_showFires ? nFire : 0) + (s_showQuakes ? nQuake : 0);
    if (visible == 0 && (s_showFires || s_showQuakes)) {
        if (g_firesPending || g_mapQuakesPending) drawStatus(tft, "FETCHING...");
        else if (!wifiOk)                          drawStatus(tft, "OFFLINE");
        else                                       drawStatus(tft, "NO DATA");
    }

    // ── Floating legend buttons (ocean corners, same line) ────────────────
    char fBuf[16], qBuf[16];
    if (nFire == 0 && g_firesPending) snprintf(fBuf, sizeof(fBuf), "FIRE --");
    else                              snprintf(fBuf, sizeof(fBuf), "FIRE %d", nFire);
    if (nQuake == 0 && g_mapQuakesPending) snprintf(qBuf, sizeof(qBuf), "QUAKE --");
    else                                   snprintf(qBuf, sizeof(qBuf), "QUAKE %d", nQuake);

    s_fireLegendW  = legendWidth(tft, fBuf);
    s_quakeLegendW = legendWidth(tft, qBuf);
    s_fireLegendX  = LEGEND_MARGIN;
    s_quakeLegendX = SCREEN_W - LEGEND_MARGIN - s_quakeLegendW;

    drawLegend(tft, s_fireLegendX, LEGEND_Y, s_fireLegendW, fBuf, s_showFires, 0);
    drawLegend(tft, s_quakeLegendX, LEGEND_Y, s_quakeLegendW, qBuf, s_showQuakes, 1);
}

// ── Tap ─────────────────────────────────────────────────────────────────────
bool screenMapTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    (void)tft;
    layersLoad();

    // Top bar: force both layers to re-fetch on the next draw.
    if (y < TOPBAR_H && wifiOk) {
        firesMapForceRefresh();
        s_mqFetchedMs = 0;
        s_mqForce = true;
        return true;
    }

    // Floating legend buttons — 4 px of vertical slack for fingers.
    if (y >= LEGEND_Y - 4 && y < LEGEND_Y + LEGEND_H + 4) {
        if (s_fireLegendW > 0 &&
            x >= s_fireLegendX && x < s_fireLegendX + s_fireLegendW) {
            s_showFires = !s_showFires;
            layersSave();
            return true;
        }
        if (s_quakeLegendW > 0 &&
            x >= s_quakeLegendX && x < s_quakeLegendX + s_quakeLegendW) {
            s_showQuakes = !s_showQuakes;
            layersSave();
            return true;
        }
    }
    return false;
}
