#include "screen_forecast.h"
#include "../theme.h"
#include "../widgets.h"
#include "../icons.h"
#include "../../config/config.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <cstdio>
#include <math.h>

#define DAY_W   (SCREEN_W / DAILY_COUNT)   // 64px per day
#define ICON_R  20

// Column layout; CONTENT_H = 194px
#define OFF_DAY   2    // FONT_MD 16px → ends 18
#define OFF_ICON  54   // icon center, r=20
#define OFF_HI    90   // FONT_LG 26px → ends 116
#define OFF_LO    120  // FONT_LG 26px → ends 146
#define OFF_WIND  152  // FONT_MD 16px → ends 168
#define OFF_COND  174  // FONT_SM 8px  → ends 182

void screenForecastDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "5-DAY", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 2, 4);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (!g_current.valid) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(60, 110);
        tft.print("No weather data");
        return;
    }

    for (int i = 0; i < DAILY_COUNT; i++) {
        int x  = i * DAY_W;
        int cx = x + DAY_W / 2;
        uint16_t bg = (i % 2 == 0) ? COL_BG : COL_ALTROW;
        tft.fillRect(x, CONTENT_Y, DAY_W, CONTENT_H, bg);

        if (i < DAILY_COUNT - 1)
            tft.drawFastVLine(x + DAY_W - 1, CONTENT_Y, CONTENT_H, COL_DIM);

        // ── Day label ─────────────────────────────────────────────────────
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, bg);
        const char *label = (i == 0) ? "NOW" : g_daily[i].day;
        int tw = tft.textWidth(label);
        tft.setCursor(cx - tw / 2, CONTENT_Y + OFF_DAY);
        tft.print(label);

        // ── Icon ──────────────────────────────────────────────────────────
        drawWmoIcon(tft, g_daily[i].weather_code, cx, CONTENT_Y + OFF_ICON, ICON_R, g_themeColor);

        char buf[12];

        // ── High temp ─────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%d\xB0", (int)roundf(g_daily[i].temp_max));
        tft.setTextFont(FONT_LG);
        tft.setTextColor(COL_WHITE, bg);
        tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, CONTENT_Y + OFF_HI);
        tft.print(buf);

        // ── Low temp ──────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%d\xB0", (int)roundf(g_daily[i].temp_min));
        tft.setTextFont(FONT_LG);
        tft.setTextColor(COL_WHITE, bg);
        tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, CONTENT_Y + OFF_LO);
        tft.print(buf);

        // ── Wind ──────────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%dmph", (int)roundf(g_daily[i].wind_speed));
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WIND, bg);
        tw = tft.textWidth(buf);
        tft.setCursor(cx - tw / 2, CONTENT_Y + OFF_WIND);
        tft.print(buf);

        // ── Condition ─────────────────────────────────────────────────────
        const char *cond = wmoDescription(g_daily[i].weather_code);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, bg);
        tw = tft.textWidth(cond);
        if (tw <= DAY_W) {
            tft.setCursor(cx - tw / 2, CONTENT_Y + OFF_COND);
            tft.print(cond);
        }
    }
}
