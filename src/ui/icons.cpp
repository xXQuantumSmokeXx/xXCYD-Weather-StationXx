#include "icons.h"
#include "theme.h"
#include <math.h>

// ── Sun ───────────────────────────────────────────────────────────────────────
static void drawSun(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    tft.fillCircle(cx, cy, r * 3 / 8, col);
    for (int i = 0; i < 8; i++) {
        float a = i * M_PI / 4.0f;
        int x0 = cx + (int)((r * 0.50f) * cosf(a));
        int y0 = cy + (int)((r * 0.50f) * sinf(a));
        int x1 = cx + (int)((r * 0.95f) * cosf(a));
        int y1 = cy + (int)((r * 0.95f) * sinf(a));
        tft.drawLine(x0, y0, x1, y1, col);
    }
}

// ── Cloud ─────────────────────────────────────────────────────────────────────
// Symmetric 3-dome cloud: big centre bump flanked by two smaller side bumps.
// cy = centre of the overall shape (base sits below cy).
static void drawCloud(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    int bigR  = r * 6 / 10;   // centre dome radius
    int smlR  = r * 38 / 100; // side dome radius (slightly smaller)
    int sOffX = r * 55 / 100; // side dome horizontal offset

    // Side domes first so centre overlaps them
    tft.fillCircle(cx - sOffX, cy + bigR / 4, smlR, col);
    tft.fillCircle(cx + sOffX, cy + bigR / 4, smlR, col);
    // Centre dome (tallest)
    tft.fillCircle(cx, cy, bigR, col);
    // Flat base connecting everything
    int lx = cx - sOffX - smlR;
    int rx = cx + sOffX + smlR;
    int by = cy + bigR / 2;
    tft.fillRect(lx, by, rx - lx + 1, (cy + bigR) - by + 1, col);
}

// ── Partly Cloudy ─────────────────────────────────────────────────────────────
static void drawPartlyCloudy(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    int sr = r * 2 / 3;
    drawSun(tft, cx + r / 4, cy - r / 4, sr, col);
    tft.fillCircle(cx - r / 5, cy + r / 4, r / 3, COL_BG);  // erase sun overlap
    drawCloud(tft, cx - r / 5, cy + r / 4, sr, col);
}

// ── Rain ──────────────────────────────────────────────────────────────────────
// Straight vertical 2px-wide drops — clean, modern look
static void drawRain(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    drawCloud(tft, cx, cy - r / 4, r, col);
    int dropY   = cy + r / 4;
    int dropLen = r * 5 / 8;
    for (int i = -1; i <= 1; i++) {
        int dx = cx + i * (r / 3);
        tft.drawFastVLine(dx,     dropY, dropLen, COL_RAIN);
        tft.drawFastVLine(dx + 1, dropY, dropLen, COL_RAIN);
    }
}

// ── Drizzle ───────────────────────────────────────────────────────────────────
static void drawDrizzle(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    drawCloud(tft, cx, cy - r / 4, r, col);
    int dotY = cy + r / 3;
    for (int i = -1; i <= 1; i++)
        tft.fillCircle(cx + i * (r / 3), dotY, 2, COL_RAIN);
}

// ── Snow ──────────────────────────────────────────────────────────────────────
// Classic 6-armed asterisk snowflake with side branches
static void drawSnow(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    for (int i = 0; i < 6; i++) {
        float a  = i * M_PI / 3.0f;
        float ca = cosf(a), sa = sinf(a);
        tft.drawLine(cx, cy, cx + (int)(r * ca), cy + (int)(r * sa), col);
        // Side branches at 55% of arm length, ±60° from arm
        float bDist = 0.55f * r;
        int mx = cx + (int)(bDist * ca);
        int my = cy + (int)(bDist * sa);
        for (int s = -1; s <= 1; s += 2) {
            float ba = a + s * M_PI / 3.0f;
            tft.drawLine(mx, my,
                mx + (int)(r * 0.32f * cosf(ba)),
                my + (int)(r * 0.32f * sinf(ba)), col);
        }
    }
    tft.fillCircle(cx, cy, r / 7 + 1, col);
}

// ── Fog ───────────────────────────────────────────────────────────────────────
// Five horizontal bands with graduated lengths
static void drawFog(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    static const float wf[] = { 0.65f, 1.0f, 0.85f, 0.70f, 0.50f };
    int spacing = r / 3;
    if (spacing < 3) spacing = 3;
    for (int i = 0; i < 5; i++) {
        int y  = cy + (i - 2) * spacing;
        int hw = (int)(r * wf[i]);
        tft.drawFastHLine(cx - hw, y, hw * 2, col);
    }
}

// ── Storm ─────────────────────────────────────────────────────────────────────
// Cloud + bold Z-shaped lightning bolt
static void drawStorm(TFT_eSPI &tft, int cx, int cy, int r, uint16_t col) {
    drawCloud(tft, cx, cy - r / 3, r, col);
    int bx  = cx + r / 5;
    int by  = cy + r / 5;
    int mid = by + r * 2 / 5;
    int bot = by + r * 4 / 5;
    int lx  = bx - r / 3;     // left x at bend points
    // Draw bolt twice (side by side) for 2px visual weight
    for (int d = 0; d <= 1; d++) {
        tft.drawLine(bx + d, by,  lx + d, mid, COL_STORM);
        tft.drawLine(lx + d, mid, bx + d, mid, COL_STORM);
        tft.drawLine(bx + d, mid, lx + d, bot, COL_STORM);
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────────
void drawWeatherIcon(TFT_eSPI &tft, WxIcon icon, int cx, int cy, int r, uint16_t col) {
    switch (icon) {
        case WxIcon::Sun:          drawSun(tft, cx, cy, r, col);          break;
        case WxIcon::PartlyCloudy: drawPartlyCloudy(tft, cx, cy, r, col); break;
        case WxIcon::Cloudy:       drawCloud(tft, cx, cy, r, col);        break;
        case WxIcon::Fog:          drawFog(tft, cx, cy, r, col);          break;
        case WxIcon::Drizzle:      drawDrizzle(tft, cx, cy, r, col);      break;
        case WxIcon::Rain:         drawRain(tft, cx, cy, r, col);         break;
        case WxIcon::Snow:         drawSnow(tft, cx, cy, r, col);         break;
        case WxIcon::Showers:      drawRain(tft, cx, cy, r, col);         break;
        case WxIcon::Storm:        drawStorm(tft, cx, cy, r, col);        break;
    }
}
