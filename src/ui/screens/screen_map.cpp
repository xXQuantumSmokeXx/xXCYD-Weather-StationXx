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
// the *aspect*, not in the x scale.  The map owns the full content width and
// the last 14 px of the content column are the layer bar, so the map starts
// 10 px under the top bar instead of 15 to make that column add up exactly:
// 33 + 170 = 203, bar 203..216, bottom bar at 218.
#define MAP_LON_MIN  (-125.0f)
#define MAP_LON_MAX   (-66.0f)
#define MAP_LAT_MIN    (24.5f)
#define MAP_LAT_MAX    (49.5f)
#define MAP_X0         0
#define MAP_Y0         (CONTENT_Y + 10)
#define MAP_W          320
#define MAP_H          170
#define MAP_X_SCALE    (MAP_W / (MAP_LON_MAX - MAP_LON_MIN))
#define MAP_Y_SCALE    (MAP_H / (MAP_LAT_MAX - MAP_LAT_MIN))

// Must match FIRE_MAX in screen_fires.cpp.
#define FIRE_MAP_MAX   40

// ── Layer bar — four equal touch cells in one line under the map ───────────
#define LEGEND_Y       (MAP_Y0 + MAP_H)          // 203
#define LEGEND_H       14
#define LEGEND_CELL    (SCREEN_W / 4)            // 80 px, evenly spaced
#define LEGEND_ICON    8
#define LEGEND_GAP     3

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

// ── Map outage data (/gridmap — counties at or above the Worker's threshold) ─
#define MAPG_MAX       80
#define MAPG_CACHE_MS  (10UL * 60UL * 1000UL)

struct MapOutage {
    int16_t  lat100;
    int16_t  lon100;
    uint16_t customers;      // saturated to uint16; only the 10k class is drawn
};

static MapOutage s_mg[MAPG_MAX];
static int s_mgCount = 0;
static bool s_mgFetchedOnce = false;
static unsigned long s_mgFetchedMs = 0;
static unsigned long s_mgLastAttempt = 0;
bool g_mapGridPending = false;

// ── Map storm data (/storms — warnings only) ────────────────────────────────
#define MAPS_MAX       48
#define MAPS_CACHE_MS  (5UL * 60UL * 1000UL)

struct MapStorm {
    int16_t lat100;
    int16_t lon100;
    uint8_t sev;             // 3 tornado, 2 severe thunderstorm, 1 flash flood
};

static MapStorm s_ms[MAPS_MAX];
static int s_msCount = 0;
static bool s_msFetchedOnce = false;
static unsigned long s_msFetchedMs = 0;
static unsigned long s_msLastAttempt = 0;
bool g_mapStormsPending = false;

// ── Layer toggles (NVS-backed, all four on by default) ─────────────────────
static bool s_showFires = true;
static bool s_showQuakes = true;
static bool s_showGrid = true;
static bool s_showStorms = true;
static bool s_layersLoaded = false;

#define LAYER_FIRE   1
#define LAYER_QUAKE  2
#define LAYER_GRID   4
#define LAYER_STORM  8
#define LAYER_ALL    (LAYER_FIRE | LAYER_QUAKE | LAYER_GRID | LAYER_STORM)

static void layersLoad() {
    if (s_layersLoaded) return;
    // -1 means "never saved": an upgrade from the two-layer MAP must start
    // with all four on rather than inheriting a stored 0..3 mask.
    int v = nvsGetInt("map_lay", -1);
    if (v < 0) v = LAYER_ALL;
    s_showFires  = (v & LAYER_FIRE) != 0;
    s_showQuakes = (v & LAYER_QUAKE) != 0;
    s_showGrid   = (v & LAYER_GRID) != 0;
    s_showStorms = (v & LAYER_STORM) != 0;
    s_layersLoaded = true;
}

static void layersSave() {
    nvsPutInt("map_lay", (s_showFires  ? LAYER_FIRE  : 0) |
                         (s_showQuakes ? LAYER_QUAKE : 0) |
                         (s_showGrid   ? LAYER_GRID  : 0) |
                         (s_showStorms ? LAYER_STORM : 0));
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
// 8×8 glyphs, drawn as horizontal runs (one fillRect per run, not per pixel).
// Bit 7 is the leftmost pixel.
static const uint8_t FLAME[8] = {
    0x08, 0x18, 0x38, 0x7C, 0x7C, 0x7C, 0x38, 0x00
};
static const uint8_t BOLT[8] = {          // GRID — power outage
    0x18, 0x30, 0x60, 0x7E, 0x0C, 0x18, 0x30, 0x00
};
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

static void drawFlame(TFT_eSPI &tft, int cx, int cy, uint16_t col) {
    drawGlyph(tft, FLAME, cx, cy, col);
}

// Semantic marker colors — fixed, not theme-derived, so the layers stay
// distinct on every theme and in e-ink invert.
#define MAP_COL_FIRE   0xFD20u   // bright orange
#define MAP_COL_QUAKE  0x3FE2u   // neon green
#define MAP_COL_GRID   0xFFE0u   // amber — power
#define MAP_COL_STORM  0xF800u   // red — warnings

static void drawQuake(TFT_eSPI &tft, int cx, int cy, float mag) {
    int r = mag >= 6.0f ? 6 : mag >= 5.0f ? 5 : mag >= 4.0f ? 4 : 3;
    tft.drawCircle(cx, cy, r, MAP_COL_QUAKE);
    if (mag >= 5.0f) tft.drawPixel(cx, cy, MAP_COL_QUAKE);
}

// One county outage.  Every marker is the same glyph so the layer reads as a
// single symbol; a ring marks the ones big enough to matter at national scale.
static void drawOutage(TFT_eSPI &tft, int cx, int cy, uint16_t customers) {
    drawGlyph(tft, BOLT, cx, cy, MAP_COL_GRID);
    if (customers >= 10000) tft.drawCircle(cx, cy, 5, MAP_COL_GRID);
}

// A triangle, not a ring: rings already mean earthquakes, and a hazard sign
// reads as a warning at a glance.  Severity is fill then size — a tornado
// warning is filled, a severe thunderstorm is outlined with a core pixel, a
// flash flood warning is the small outline alone.
static void drawStorm(TFT_eSPI &tft, int cx, int cy, int sev) {
    int r = sev >= 3 ? 5 : sev == 2 ? 4 : 3;
    int top = cy - r, bot = cy + r;
    if (sev >= 3) {
        tft.fillTriangle(cx, top, cx - r, bot, cx + r, bot, MAP_COL_STORM);
    } else {
        tft.drawTriangle(cx, top, cx - r, bot, cx + r, bot, MAP_COL_STORM);
        if (sev == 2) tft.drawPixel(cx, cy, MAP_COL_STORM);
    }
}

static void drawHome(TFT_eSPI &tft, int cx, int cy) {
    uint16_t col = COL_WHITE;
    tft.drawFastHLine(cx - 4, cy, 3, col);
    tft.drawFastHLine(cx + 2, cy, 3, col);
    tft.drawFastVLine(cx, cy - 4, 3, col);
    tft.drawFastVLine(cx, cy + 2, 3, col);
}

// ── Declutter: skip a mark that would sit on one already drawn ─────────────
#define MAX_MARKS 128
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

// ── Layer bar ───────────────────────────────────────────────────────────────
// Borderless black cells: the text always stays theme-colored; the icon is
// semantic when the layer is on and dim when it is off.  kind: 0 fire,
// 1 quake, 2 grid, 3 storm — each icon matches the marker it stands for.
static void drawLegendCell(TFT_eSPI &tft, int idx, const char *label,
                           const char *count, bool on, int kind) {
    int x = idx * LEGEND_CELL;
    tft.fillRect(x, LEGEND_Y, LEGEND_CELL, LEGEND_H, COL_BG);

    char buf[16];
    snprintf(buf, sizeof(buf), "%s %s", label, count);
    tft.setTextFont(FONT_SM);
    int total = LEGEND_ICON + LEGEND_GAP + tft.textWidth(buf);
    int bx = x + (LEGEND_CELL - total) / 2;      // evenly spaced within the cell
    int cy = LEGEND_Y + LEGEND_H / 2;

    uint16_t iconCol = !on ? COL_DIM
                    : kind == 0 ? MAP_COL_FIRE
                    : kind == 1 ? MAP_COL_QUAKE
                    : kind == 2 ? MAP_COL_GRID
                                : MAP_COL_STORM;
    int icx = bx + LEGEND_ICON / 2;
    switch (kind) {
        case 0: drawFlame(tft, icx, cy, iconCol); break;
        case 1: tft.drawCircle(icx, cy, 3, iconCol); break;
        case 2: drawGlyph(tft, BOLT, icx, cy, iconCol); break;
        default: tft.drawTriangle(icx, cy - 4, icx - 4, cy + 4, icx + 4, cy + 4,
                                  iconCol); break;
    }

    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(bx + LEGEND_ICON + LEGEND_GAP, LEGEND_Y + (LEGEND_H - 8) / 2);
    tft.print(buf);
}

// Hairline between cells so four labels read as four buttons, and a rule
// along the top so the bar is not mistaken for map content.
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
    tft.setCursor((SCREEN_W - tw) / 2, MAP_Y0 + 78);
    tft.print(msg);
}

// ── Fetch ───────────────────────────────────────────────────────────────────
// One GET + JSON parse shared by all three map feeds.  Chunked replies must be
// read through writeToStream (HTTPClient::getString reserves _size+1 == 1).
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

static bool mapQStale() {
    if (s_mqForce) return true;
    if (!s_mqFetchedOnce) return millis() - s_mqLastAttempt > 30000UL;
    return millis() - s_mqFetchedMs > MAPQ_CACHE_MS;
}

static bool mapGridStale() {
    if (!s_mgFetchedOnce) return millis() - s_mgLastAttempt > 30000UL;
    return millis() - s_mgFetchedMs > MAPG_CACHE_MS;
}

static bool mapStormsStale() {
    if (!s_msFetchedOnce) return millis() - s_msLastAttempt > 20000UL;
    return millis() - s_msFetchedMs > MAPS_CACHE_MS;
}

bool mapQuakesFetch(bool wifiOk) {
    s_mqLastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) return false;

    JsonDocument doc;
    if (!httpGetJson("http://quantum-meteor.qsmoke.workers.dev/quakes?us=1&min=3.5&days=30",
                     doc)) return false;
    JsonArray arr = doc["items"].as<JsonArray>();
    if (arr.isNull()) return false;

    int count = 0;
    for (JsonObject it : arr) {
        if (count >= MAPQ_MAX) break;
        if (it["la"].isNull() || it["lo"].isNull()) continue;
        s_mq[count].lat100 = to100(it["la"] | 0.0f);
        s_mq[count].lon100 = to100(it["lo"] | 0.0f);
        s_mq[count].mag10  = (uint8_t)((it["m"] | 0.0f) * 10.0f + 0.5f);
        count++;
    }
    if (count == 0) return false;

    s_mqCount = count;
    s_mqFetchedOnce = true;
    s_mqFetchedMs = millis();
    return true;
}

// An empty list is a valid answer here: no county is over the threshold.  The
// Worker already filtered and aggregated, so the device stores nothing extra.
bool mapGridFetch(bool wifiOk) {
    s_mgLastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) return false;

    JsonDocument doc;
    if (!httpGetJson("http://quantum-meteor.qsmoke.workers.dev/gridmap", doc)) return false;
    JsonArray arr = doc["items"].as<JsonArray>();
    if (arr.isNull()) return false;

    int count = 0;
    for (JsonObject it : arr) {
        if (count >= MAPG_MAX) break;
        if (it["la"].isNull() || it["lo"].isNull()) continue;
        s_mg[count].lat100 = to100(it["la"] | 0.0f);
        s_mg[count].lon100 = to100(it["lo"] | 0.0f);
        long o = it["o"] | 0L;
        s_mg[count].customers = (uint16_t)(o > 65535 ? 65535 : (o < 0 ? 0 : o));
        count++;
    }

    s_mgCount = count;
    s_mgFetchedOnce = true;
    s_mgFetchedMs = millis();
    return true;
}

bool mapStormsFetch(bool wifiOk) {
    s_msLastAttempt = millis();
    if (!wifiOk || !WiFi.isConnected()) return false;

    JsonDocument doc;
    if (!httpGetJson("http://quantum-meteor.qsmoke.workers.dev/storms", doc)) return false;
    JsonArray arr = doc["items"].as<JsonArray>();
    if (arr.isNull()) return false;

    int count = 0;
    for (JsonObject it : arr) {
        if (count >= MAPS_MAX) break;
        if (it["la"].isNull() || it["lo"].isNull()) continue;
        s_ms[count].lat100 = to100(it["la"] | 0.0f);
        s_ms[count].lon100 = to100(it["lo"] | 0.0f);
        int sev = it["s"] | 1;
        s_ms[count].sev = (uint8_t)(sev < 1 ? 1 : sev > 3 ? 3 : sev);
        count++;
    }

    s_msCount = count;
    s_msFetchedOnce = true;
    s_msFetchedMs = millis();
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
    if (wifiOk && !g_mapGridPending && mapGridStale())   triggerMapGridFetch();
    if (wifiOk && !g_mapStormsPending && mapStormsStale()) triggerMapStormsFetch();

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

    Mark gMarks[MAPG_MAX];
    int nGrid = 0;
    for (int i = 0; i < s_mgCount; i++) {
        int x, y;
        if (!projectIn(s_mg[i].lon100 / 100.0f, s_mg[i].lat100 / 100.0f, x, y)) continue;
        gMarks[nGrid].x = x;
        gMarks[nGrid].y = y;
        gMarks[nGrid].sev = s_mg[i].customers;
        nGrid++;
    }
    sortDesc(gMarks, nGrid);

    Mark sMarks[MAPS_MAX];
    int nStorm = 0;
    for (int i = 0; i < s_msCount; i++) {
        int x, y;
        if (!projectIn(s_ms[i].lon100 / 100.0f, s_ms[i].lat100 / 100.0f, x, y)) continue;
        sMarks[nStorm].x = x;
        sMarks[nStorm].y = y;
        sMarks[nStorm].sev = s_ms[i].sev;
        nStorm++;
    }
    sortDesc(sMarks, nStorm);

    // ── Marks, most acute layer first so it wins the declutter ring ───────
    s_mn = 0;
    if (s_showStorms) {
        for (int i = 0; i < nStorm; i++) {
            if (!canPlace(sMarks[i].x, sMarks[i].y, 12)) continue;
            drawStorm(tft, sMarks[i].x, sMarks[i].y, sMarks[i].sev);
            place(sMarks[i].x, sMarks[i].y);
        }
    }
    if (s_showQuakes) {
        for (int i = 0; i < nQuake; i++) {
            if (!canPlace(qMarks[i].x, qMarks[i].y, 11)) continue;
            drawQuake(tft, qMarks[i].x, qMarks[i].y, qMarks[i].sev / 10.0f);
            place(qMarks[i].x, qMarks[i].y);
        }
    }
    if (s_showGrid) {
        for (int i = 0; i < nGrid; i++) {
            if (!canPlace(gMarks[i].x, gMarks[i].y, 9)) continue;
            drawOutage(tft, gMarks[i].x, gMarks[i].y, gMarks[i].sev);
            place(gMarks[i].x, gMarks[i].y);
        }
    }
    if (s_showFires) {
        for (int i = 0; i < nFire; i++) {
            if (!canPlace(fMarks[i].x, fMarks[i].y, 7)) continue;
            drawFlame(tft, fMarks[i].x, fMarks[i].y, MAP_COL_FIRE);
            place(fMarks[i].x, fMarks[i].y);
        }
    }

    // Home marker last so it stays visible.
    if (g_location.valid) {
        int hx, hy;
        if (projectIn(g_location.lon, g_location.lat, hx, hy)) drawHome(tft, hx, hy);
    }

    // ── Status when the visible map is empty ──────────────────────────────
    int visible = (s_showFires  ? nFire  : 0) + (s_showQuakes ? nQuake : 0) +
                  (s_showGrid   ? nGrid  : 0) + (s_showStorms ? nStorm : 0);
    if (visible == 0 && (s_showFires || s_showQuakes || s_showGrid || s_showStorms)) {
        if (g_firesPending || g_mapQuakesPending || g_mapGridPending ||
            g_mapStormsPending)                    drawStatus(tft, "FETCHING...");
        else if (!wifiOk)                          drawStatus(tft, "OFFLINE");
        else                                       drawStatus(tft, "NO DATA");
    }

    // ── Layer bar ─────────────────────────────────────────────────────────
    char fBuf[8], qBuf[8], gBuf[8], sBuf[8];
    if (nFire  == 0 && g_firesPending)      snprintf(fBuf, sizeof(fBuf), "--");
    else                                    snprintf(fBuf, sizeof(fBuf), "%d", nFire);
    if (nQuake == 0 && g_mapQuakesPending)  snprintf(qBuf, sizeof(qBuf), "--");
    else                                    snprintf(qBuf, sizeof(qBuf), "%d", nQuake);
    if (nGrid  == 0 && g_mapGridPending)    snprintf(gBuf, sizeof(gBuf), "--");
    else                                    snprintf(gBuf, sizeof(gBuf), "%d", nGrid);
    if (nStorm == 0 && g_mapStormsPending)  snprintf(sBuf, sizeof(sBuf), "--");
    else                                    snprintf(sBuf, sizeof(sBuf), "%d", nStorm);

    drawLegendCell(tft, 0, "FIRE",  fBuf, s_showFires,  0);
    drawLegendCell(tft, 1, "QUAKE", qBuf, s_showQuakes, 1);
    drawLegendCell(tft, 2, "GRID",  gBuf, s_showGrid,   2);
    drawLegendCell(tft, 3, "STORM", sBuf, s_showStorms, 3);
    drawLegendFrame(tft);
}

// ── Tap ─────────────────────────────────────────────────────────────────────
bool screenMapIsControlTap(int16_t y) {
    return y < TOPBAR_H ||
           (y >= LEGEND_Y - 4 && y < LEGEND_Y + LEGEND_H + 4);
}

bool screenMapTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    (void)tft;
    layersLoad();

    // Top bar: force every layer to re-fetch on the next draw.
    if (y < TOPBAR_H && wifiOk) {
        firesMapForceRefresh();
        s_mqFetchedMs = 0;
        s_mqForce = true;
        s_mgFetchedMs = 0;
        s_msFetchedMs = 0;
        return true;
    }

    // Layer bar — the whole cell is the touch target; 4 px of vertical slack
    // for fingers.
    if (y >= LEGEND_Y - 4 && y < LEGEND_Y + LEGEND_H + 4) {
        if (x < 0 || x >= SCREEN_W) return false;
        int cell = x / LEGEND_CELL;
        switch (cell) {
            case 0: s_showFires  = !s_showFires;  break;
            case 1: s_showQuakes = !s_showQuakes; break;
            case 2: s_showGrid   = !s_showGrid;   break;
            default: s_showStorms = !s_showStorms; break;
        }
        layersSave();
        return true;
    }
    return false;
}
