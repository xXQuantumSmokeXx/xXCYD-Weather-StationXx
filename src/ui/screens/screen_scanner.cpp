#include "screen_scanner.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

// ── AP entry ────────────────────────────────────────────────────────────
struct ApEntry {
    char ssid[33];
    int8_t rssi;
    int  age;   // frames since last seen; 0 = fresh
    int  dist;  // mapped pixel distance from center (cached for sorting)
};

static ApEntry s_aps[30];
static int s_apCount = 0;
static int s_scanState = 0;        // 0=idle, 1=scanning
static unsigned long s_lastScanMs = 0;
static float s_sweepDeg = 0.0f;
static TFT_eSprite *s_spr = nullptr;

static const int  SCAN_INTERVAL_MS = 5000;
static const float SWEEP_DEG_PER_FRAME = 2.0f;
static const int  MAX_RADIUS = 82;
static const int  RING_COUNT = 4;
static const int  SPR_SIZE = MAX_RADIUS * 2 + 10;    // 174
static const int  SPR_X = 160 - SPR_SIZE / 2;         // 73
static const int  SPR_Y = CONTENT_Y + CONTENT_H / 2 - SPR_SIZE / 2;

// djb2 hash
static unsigned djb2(const char *s) {
    unsigned h = 5381;
    for (; *s; s++) h = ((h << 5) + h) + (unsigned char)*s;
    return h;
}

// RSSI → pixel distance; clamped to ring bounds
static int rssiToDist(int8_t rssi) {
    int d = map(rssi, -90, -30, MAX_RADIUS, MAX_RADIUS / RING_COUNT);
    if (d < MAX_RADIUS / RING_COUNT) d = MAX_RADIUS / RING_COUNT;
    if (d > MAX_RADIUS) d = MAX_RADIUS;
    return d;
}

// True if this AP maps into the two inner rings
static bool isInner(const ApEntry &ap) {
    return ap.dist <= MAX_RADIUS * 2 / RING_COUNT;   // ≤ 41
}

// Sort by RSSI (strongest first)
static int cmpRssi(const void *a, const void *b) {
    return ((const ApEntry*)a)->rssi - ((const ApEntry*)b)->rssi;
}

// ── Sweep only — called every animation frame for smooth movement ──────
static void tickSweep() {
    s_sweepDeg += SWEEP_DEG_PER_FRAME;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
}

// ── State tick: age, scan — called at a throttled rate ──────────────────
static bool s_scanJustFinished = false;
static void tickState() {
    // Age entries (once per call; call is throttled externally)
    for (int i = 0; i < s_apCount; i++) {
        s_aps[i].age++;
        if (s_aps[i].age > 20) {
            if (i < s_apCount - 1) s_aps[i] = s_aps[s_apCount - 1];
            s_apCount--;
            i--;
        }
    }

    if (s_scanState == 0) {
        if (millis() - s_lastScanMs >= SCAN_INTERVAL_MS) {
            WiFi.scanNetworks(true, false);
            s_scanState = 1;
            s_lastScanMs = millis();
        }
    } else {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            s_scanState = 0;
            s_scanJustFinished = true;
            if (n > 30) n = 30;
            s_apCount = 0;
            for (int i = 0; i < n && s_apCount < 30; i++) {
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) continue;
                ApEntry ent;
                strncpy(ent.ssid, ssid.c_str(), 32);
                ent.ssid[32] = '\0';
                ent.rssi = WiFi.RSSI(i);
                ent.age = 0;
                ent.dist = rssiToDist(ent.rssi);

                bool found = false;
                for (int j = 0; j < s_apCount; j++) {
                    if (strcmp(s_aps[j].ssid, ent.ssid) == 0) {
                        s_aps[j].rssi = ent.rssi;
                        s_aps[j].dist  = ent.dist;
                        s_aps[j].age  = 0;
                        found = true;
                        break;
                    }
                }
                if (!found) s_aps[s_apCount++] = ent;
            }
            // Sort strongest-first
            qsort(s_aps, s_apCount, sizeof(ApEntry), cmpRssi);
            WiFi.scanDelete();
        }
    }
}

// ── Render radar frame into the sprite ──────────────────────────────────
static void renderFrame(TFT_eSprite &spr) {
    const int C = SPR_SIZE / 2;

    spr.fillSprite(COL_BG);

    // Rings: inner 2 themed, outer 2 dim
    for (int r = 1; r <= RING_COUNT; r++) {
        int radius = MAX_RADIUS * r / RING_COUNT;
        uint16_t rc = (r <= 2) ? g_themeColor : COL_DIM;
        spr.drawCircle(C, C, radius, rc);
    }
    // Crosshairs
    spr.drawFastHLine(C - MAX_RADIUS, C, MAX_RADIUS * 2, COL_DIM);
    spr.drawFastVLine(C, C - MAX_RADIUS, MAX_RADIUS * 2, COL_DIM);
    // Center dot
    spr.fillCircle(C, C, 2, g_themeColor);

    float sweepAng = fmodf(s_sweepDeg, 360.0f);

    // Dots + labels
    for (int i = 0; i < s_apCount; i++) {
        ApEntry &ap = s_aps[i];

        float dotAng = fmodf((float)(djb2(ap.ssid) % 360), 360.0f);
        float rad = dotAng * M_PI / 180.0f;
        int dx = C + (int)(ap.dist * cosf(rad));
        int dy = C - (int)(ap.dist * sinf(rad));

        // Pulse: sweep proximity to dot angle
        float diff = fabsf(sweepAng - dotAng);
        if (diff > 180.0f) diff = 360.0f - diff;
        float pulse = (diff < 28.0f) ? (1.0f - diff / 28.0f) : 0.0f;

        int r = 2 + (int)(pulse * 2.5f);
        if (ap.age < 2 && r < 3) r = 3;

        // All nodes use theme color — no dim dots
        uint16_t dotCol = g_themeColor;

        spr.fillCircle(dx, dy, r, dotCol);

        // Label only when sweep bar passes over the node
        if (pulse > 0.15f) {
            char label[14];
            int len = strlen(ap.ssid);
            if (len > 12) { memcpy(label, ap.ssid, 11); label[11] = '.'; label[12] = '.'; label[13] = '\0'; }
            else { strcpy(label, ap.ssid); }

            // Themed text — no dim/white
            spr.setTextFont(1);
            spr.setTextColor(g_themeColor, COL_BG);
            int lw = spr.textWidth(label);
            int lx = dx - lw / 2;
            int ly = dy - 10;
            if (lx < 2) lx = 2;
            if (lx + lw > SPR_SIZE - 2) lx = SPR_SIZE - 2 - lw;
            if (ly < 2) ly = dy + 4;
            if (ly > SPR_SIZE - 9) ly = SPR_SIZE - 9;
            spr.setCursor(lx, ly);
            spr.print(label);
        }
    }

    // Sweep line
    float swRad = sweepAng * M_PI / 180.0f;
    int sx = C + (int)(MAX_RADIUS * cosf(swRad));
    int sy = C - (int)(MAX_RADIUS * sinf(swRad));
    spr.drawLine(C, C, sx, sy, g_themeColor);
}

// ── Draw side lists (left=outer, right=inner) ──────────────────────────
static void drawSideLists(TFT_eSPI &tft) {
    tft.setTextFont(1);
    const int lineH = 10;
    const int maxShow = 7;   // max entries per side

    // ── Right column: INNER (inner 2 rings) ─────────────────────────────
    {
        int x = SPR_X + SPR_SIZE + 3;   // 250

        // Count and pre-measure height for vertical centering
        int innerCount = 0;
        for (int i = 0; i < s_apCount; i++)
            if (isInner(s_aps[i])) innerCount++;
        int show = (innerCount < maxShow) ? innerCount : maxShow;
        int totalH = 14 + show * lineH;  // header + entries
        int y = CONTENT_Y + (CONTENT_H - totalH) / 2;

        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(x, y);
        tft.print("INNER");
        y += lineH + 4;

        if (show == 0) {
            tft.setTextColor(COL_WHITE, COL_BG);
            tft.setCursor(x, y);
            tft.print("--");
        } else {
            int shown = 0;
            for (int i = 0; i < s_apCount && shown < maxShow; i++) {
                if (!isInner(s_aps[i])) continue;
                if (y > SCREEN_H - BOTBAR_H - 14) break;

                char buf[14];
                int len = strlen(s_aps[i].ssid);
                if (len > 11) { memcpy(buf, s_aps[i].ssid, 10); buf[10] = '.'; buf[11] = '\0'; }
                else { strcpy(buf, s_aps[i].ssid); }

                tft.setTextColor(g_themeColor, COL_BG);
                tft.setCursor(x, y);
                tft.print(buf);

                y += lineH;
                shown++;
            }
        }
    }

    // ── Left column: OUTER (outer 2 rings) ──────────────────────────────
    {
        int x = 2;

        int outerCount = s_apCount;
        for (int i = 0; i < s_apCount; i++)
            if (isInner(s_aps[i])) outerCount--;
        int show = (outerCount < maxShow) ? outerCount : maxShow;
        int totalH = 14 + show * lineH;
        int y = CONTENT_Y + (CONTENT_H - totalH) / 2;

        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(x, y);
        tft.print("OUTER");
        y += lineH + 4;

        if (show == 0) {
            tft.setTextColor(COL_WHITE, COL_BG);
            tft.setCursor(x, y);
            tft.print("--");
        } else {
            int shown = 0;
            for (int i = 0; i < s_apCount && shown < maxShow; i++) {
                if (isInner(s_aps[i])) continue;
                if (y > SCREEN_H - BOTBAR_H - 14) break;

                char buf[12];
                int len = strlen(s_aps[i].ssid);
                if (len > 10) { memcpy(buf, s_aps[i].ssid, 9); buf[9] = '.'; buf[10] = '\0'; }
                else { strcpy(buf, s_aps[i].ssid); }

                tft.setTextColor(g_themeColor, COL_BG);
                tft.setCursor(x, y);
                tft.print(buf);

                y += lineH;
                shown++;
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
//  Public API
// ═════════════════════════════════════════════════════════════════════════

void screenScannerDraw(TFT_eSPI &tft, bool wifiOk) {
    if (s_spr) { s_spr->deleteSprite(); delete s_spr; s_spr = nullptr; }

    char timeStr[10], dateStr[32];
    timeGetShort(timeStr);
    timeGetDateLong(dateStr, sizeof(dateStr));

    drawTopbar(tft, g_location.valid ? g_location.city : "", "SCANNER", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 10, 12);

    // Clear full content area
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    s_spr = new TFT_eSprite(&tft);
    s_spr->setColorDepth(16);
    s_spr->createSprite(SPR_SIZE, SPR_SIZE);
    renderFrame(*s_spr);
    s_spr->pushSprite(SPR_X, SPR_Y);

    drawSideLists(tft);
    s_scanJustFinished = false;

    // AP count top-right — themed
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d APs", s_apCount);
    tft.setTextFont(1);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SCREEN_W - tft.textWidth(countBuf) - 4, CONTENT_Y + 3);
    tft.print(countBuf);

    if (s_scanState == 1) {
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(4, SCREEN_H - BOTBAR_H - 12);
        tft.print("scanning...");
    }

    tickSweep();
    tickState();
}

void screenScannerAnimate(TFT_eSPI &tft) {
    if (!s_spr) {
        s_spr = new TFT_eSprite(&tft);
        s_spr->setColorDepth(16);
        s_spr->createSprite(SPR_SIZE, SPR_SIZE);
    }

    // Always advance sweep for smooth radar animation
    tickSweep();

    // Throttle state management to ~500ms (not every 80ms animation frame)
    static unsigned long lastStateTick = 0;
    if (millis() - lastStateTick > 500) {
        tickState();
        lastStateTick = millis();
    }

    renderFrame(*s_spr);
    s_spr->pushSprite(SPR_X, SPR_Y);

    // Only redraw side lists when scan results changed — avoids flicker/doubling
    if (s_scanJustFinished) {
        // Clear side column areas before redraw
        int leftW  = 90;
        int rightX = SPR_X + SPR_SIZE + 3;
        int rightW = SCREEN_W - rightX - 2;
        tft.fillRect(2, CONTENT_Y, leftW, CONTENT_H, COL_BG);
        tft.fillRect(rightX, CONTENT_Y, rightW, CONTENT_H, COL_BG);
        drawSideLists(tft);
        s_scanJustFinished = false;
    }

    // AP count top-right — themed, clear old first
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d APs", s_apCount);
    tft.setTextFont(1);
    int cw = tft.textWidth(countBuf);
    tft.fillRect(SCREEN_W - cw - 8, CONTENT_Y + 2, cw + 8, 10, COL_BG);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SCREEN_W - cw - 4, CONTENT_Y + 3);
    tft.print(countBuf);

    // Scanning indicator — themed, clear old first
    tft.fillRect(0, SCREEN_H - BOTBAR_H - 13, 85, 16, COL_BG);
    if (s_scanState == 1) {
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(4, SCREEN_H - BOTBAR_H - 12);
        tft.print("scanning...");
    }
}
