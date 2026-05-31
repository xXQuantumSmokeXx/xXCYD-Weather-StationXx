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

// ── AP entry ────────────────────────────────────────────────────────────
struct ApEntry {
    char ssid[33];
    int8_t rssi;
    int age;   // frames since last seen; 0 = fresh
};

static ApEntry s_aps[30];
static int s_apCount = 0;
static int s_scanState = 0;        // 0=idle, 1=scanning
static unsigned long s_lastScanMs = 0;
static float s_sweepDeg = 0.0f;

static const int  SCAN_INTERVAL_MS = 5000;
static const float SWEEP_DEG_PER_FRAME = 2.0f;
static const int  MAX_RADIUS = 85;
static const int  RING_COUNT = 4;
static const int  CX = 160;
static const int  CY = CONTENT_Y + CONTENT_H / 2 - 2;   // ~118

// djb2 hash for deterministic per-AP scatter angle
static unsigned djb2(const char *s) {
    unsigned h = 5381;
    for (; *s; s++) h = ((h << 5) + h) + (unsigned char)*s;
    return h;
}

// ─────────────────────────────────────────────────────────────────────────
void screenScannerDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10], dateStr[32];
    timeGetShort(timeStr);
    timeGetDateLong(dateStr, sizeof(dateStr));

    drawTopbar(tft, g_location.valid ? g_location.city : "", "SCANNER", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 9, 10);

    // ── Content area bg ──────────────────────────────────────────────────
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    // ── Rings ────────────────────────────────────────────────────────────
    for (int r = 1; r <= RING_COUNT; r++) {
        int radius = MAX_RADIUS * r / RING_COUNT;
        tft.drawCircle(CX, CY, radius, COL_DIM);
    }

    // ── Crosshairs ───────────────────────────────────────────────────────
    tft.drawFastHLine(CX - MAX_RADIUS, CY, MAX_RADIUS * 2, COL_DIM);
    tft.drawFastVLine(CX, CY - MAX_RADIUS, MAX_RADIUS * 2, COL_DIM);

    // ── Center dot ───────────────────────────────────────────────────────
    tft.fillCircle(CX, CY, 2, g_themeColor);

    // ── Sweep line + trail ───────────────────────────────────────────────
    float rad = s_sweepDeg * M_PI / 180.0f;
    int sx = CX + (int)(MAX_RADIUS * cosf(rad));
    int sy = CY - (int)(MAX_RADIUS * sinf(rad));

    for (int t = 3; t >= 1; t--) {
        float tr = (s_sweepDeg - t * 6.0f) * M_PI / 180.0f;
        int tx = CX + (int)(MAX_RADIUS * cosf(tr));
        int ty = CY - (int)(MAX_RADIUS * sinf(tr));
        uint16_t tc = (t == 1) ? TFT_DARKGREY : ((t == 2) ? 0x4208 : COL_BG);
        tft.drawLine(CX, CY, tx, ty, tc);
    }
    tft.drawLine(CX, CY, sx, sy, g_themeColor);

    // ── Network dots ─────────────────────────────────────────────────────
    for (int i = 0; i < s_apCount; i++) {
        ApEntry &ap = s_aps[i];
        int dist = map(ap.rssi, -90, -30, MAX_RADIUS, MAX_RADIUS / RING_COUNT);
        if (dist < MAX_RADIUS / RING_COUNT) dist = MAX_RADIUS / RING_COUNT;
        if (dist > MAX_RADIUS) dist = MAX_RADIUS;

        float ang = (float)(djb2(ap.ssid) % 360) * M_PI / 180.0f;
        int dx = CX + (int)(dist * cosf(ang));
        int dy = CY - (int)(dist * sinf(ang));

        uint16_t dotCol;
        if (ap.age < 2)      dotCol = g_themeColor;
        else if (ap.age < 4) dotCol = COL_DIM;
        else                 dotCol = 0x2104;

        tft.fillCircle(dx, dy, (ap.age < 2) ? 3 : 2, dotCol);
    }

    // ── SSID labels (only on fresh dots) ────────────────────────────────
    for (int i = 0; i < s_apCount; i++) {
        ApEntry &ap = s_aps[i];
        if (ap.age >= 2) continue;

        int dist = map(ap.rssi, -90, -30, MAX_RADIUS, MAX_RADIUS / RING_COUNT);
        if (dist < MAX_RADIUS / RING_COUNT) dist = MAX_RADIUS / RING_COUNT;
        if (dist > MAX_RADIUS) dist = MAX_RADIUS;

        float ang = (float)(djb2(ap.ssid) % 360) * M_PI / 180.0f;
        int dx = CX + (int)(dist * cosf(ang));
        int dy = CY - (int)(dist * sinf(ang));

        char label[14];
        int len = strlen(ap.ssid);
        if (len > 12) { memcpy(label, ap.ssid, 11); label[11] = '.'; label[12] = '.'; label[13] = '\0'; }
        else { strcpy(label, ap.ssid); }

        tft.setTextFont(1);
        tft.setTextColor(COL_WHITE, COL_BG);
        int lw = tft.textWidth(label), lh = 8;
        int lx = dx - lw / 2;
        int ly = dy - lh - 2;
        if (lx < 2) lx = 2;
        if (lx + lw > SCREEN_W - 2) lx = SCREEN_W - 2 - lw;
        if (ly < CONTENT_Y) ly = dy + 4;
        tft.setCursor(lx, ly);
        tft.print(label);
    }

    // ── AP count ─────────────────────────────────────────────────────────
    char countBuf[16];
    snprintf(countBuf, sizeof(countBuf), "%d APs", s_apCount);
    tft.setTextFont(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(SCREEN_W - tft.textWidth(countBuf) - 4, CONTENT_Y + 3);
    tft.print(countBuf);

    // ── Scanning indicator ───────────────────────────────────────────────
    if (s_scanState == 1) {
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(4, SCREEN_H - BOTBAR_H - 12);
        tft.print("scanning...");
    }

    // ── Advance sweep angle ──────────────────────────────────────────────
    s_sweepDeg += SWEEP_DEG_PER_FRAME;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;

    // ── Age AP entries; remove stale ─────────────────────────────────────
    for (int i = 0; i < s_apCount; i++) {
        s_aps[i].age++;
        if (s_aps[i].age > 20) {
            if (i < s_apCount - 1)
                s_aps[i] = s_aps[s_apCount - 1];
            s_apCount--;
            i--;
        }
    }

    // ── WiFi scan management ─────────────────────────────────────────────
    if (s_scanState == 0) {
        if (millis() - s_lastScanMs >= SCAN_INTERVAL_MS) {
            WiFi.scanNetworks(true, false);   // async, no hidden SSIDs
            s_scanState = 1;
            s_lastScanMs = millis();
        }
    } else {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            // Scan finished — parse results
            s_scanState = 0;

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

                // Update existing or add new
                bool found = false;
                for (int j = 0; j < s_apCount; j++) {
                    if (strcmp(s_aps[j].ssid, ent.ssid) == 0) {
                        s_aps[j].rssi = ent.rssi;
                        s_aps[j].age  = 0;
                        found = true;
                        break;
                    }
                }
                if (!found) s_aps[s_apCount++] = ent;
            }

            WiFi.scanDelete();
        }
    }
}
