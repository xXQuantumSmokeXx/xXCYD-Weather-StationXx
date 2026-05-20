#include "screen_hourly.h"
#include "../theme.h"
#include "../widgets.h"
#include "../icons.h"
#include "../../config/config.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <cstdio>
#include <math.h>

// 6 cols × 2 rows = 12 hours
#define COLS   6
#define ROWS   2
#define COL_W  (SCREEN_W / COLS)          // 53px
#define ROW_H  ((CONTENT_H - 1) / ROWS)   // 96px
#define ICON_R 16

// Cell layout — hour / icon / temp / wind only; fits in 96px
#define OFF_HOUR  3    // FONT_SM 8px  → ends 11
#define OFF_ICON  30   // icon center, r=16 (top=14, bot=46) — shifted down for breathing room
#define OFF_TEMP  50   // FONT_LG 26px → ends 76
#define OFF_WIND  78   // FONT_MD 16px → ends 94

static void drawWindLegend(TFT_eSPI &tft) {
    tft.setTextFont(FONT_SM);
    int ly = SCREEN_H - BOTBAR_H + (BOTBAR_H - 8) / 2;
    const char *txt = "WIND";
    int tw = tft.textWidth(txt);
    int total = 5 + 2 + tw;
    int lx = (SCREEN_W - total) / 2;
    tft.fillRect(lx, ly, 5, 8, COL_WIND);
    tft.setTextColor(COL_WIND, COL_BG);
    tft.setCursor(lx + 7, ly);
    tft.print(txt);
}

void screenHourlyDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "HOURLY", timeStr, wifiOk);
    drawBottombar(tft, "", 1, 4);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (!g_current.valid) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(60, 110);
        tft.print("No weather data");
        return;
    }

    for (int i = 0; i < HOURLY_COUNT; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x   = col * COL_W;
        int y   = CONTENT_Y + row * ROW_H;
        int cx  = x + COL_W / 2;

        tft.fillRect(x, y, COL_W, ROW_H, COL_BG);

        // ── Hour ──────────────────────────────────────────────────────────
        int h = g_hourly[i].hour % 12;
        if (h == 0) h = 12;
        const char *ap = (g_hourly[i].hour < 12) ? "a" : "p";
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%s", h, ap);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_WHITE, COL_BG);
        int tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, y + OFF_HOUR);
        tft.print(buf);

        // ── Icon ──────────────────────────────────────────────────────────
        drawWmoIcon(tft, g_hourly[i].weather_code, cx, y + OFF_ICON, ICON_R, g_themeColor);

        // ── Temperature ───────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%d\xB0", (int)roundf(g_hourly[i].temp));
        tft.setTextFont(FONT_LG);
        tft.setTextColor(COL_WHITE, COL_BG);
        tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, y + OFF_TEMP);
        tft.print(buf);

        // ── Wind ──────────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%dmph", (int)roundf(g_hourly[i].wind_speed));
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WIND, COL_BG);
        tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, y + OFF_WIND);
        tft.print(buf);

        if (col < COLS - 1)
            tft.drawFastVLine(x + COL_W - 1, y, ROW_H, COL_DIM);
    }

    tft.drawFastHLine(0, CONTENT_Y + ROW_H, SCREEN_W, COL_DIM);
    drawWindLegend(tft);
}

bool screenHourlyTap(int16_t x, int16_t y) { return false; }
