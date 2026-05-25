#include "screen_sitrep.h"
#include "screen_solar.h"
#include "screen_fires.h"
#include "screen_usgs.h"
#include "screen_alerts.h"
#include "screen_news.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include "../../modules/weather.h"
#include <cstdio>

static void drawThreatRow(TFT_eSPI &tft, int &y, const char *label, const char *value, uint16_t color) {
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, y);
    tft.print(label);
    tft.setTextColor(color, COL_BG);
    int vw = tft.textWidth(value);
    tft.setCursor(SCREEN_W - 8 - vw, y);
    tft.print(value);
    y += 16;
}

static void drawCondRow(TFT_eSPI &tft, int &y, const char *label, const char *value) {
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(8, y);
    tft.print(label);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(68, y);
    tft.print(value);
    y += 14;
}

void screenSitrepDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SITREP", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 7, 11);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    int y = CONTENT_Y + 6;

    // ── THREATS ──────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, y);
    tft.print("THREATS");
    tft.drawFastHLine(8, y + 10, SCREEN_W - 16, g_themeColor);
    y += 16;

    char buf[64];

    // Alerts
    int alertN = screenAlertsGetCount();
    int alertSev = screenAlertsGetSevereCount();
    if (alertN > 0) {
        snprintf(buf, sizeof(buf), "%d active (%d severe)", alertN, alertSev);
        drawThreatRow(tft, y, "ALERTS:", buf, alertSev > 0 ? COL_RED : COL_AMBER);
    } else {
        drawThreatRow(tft, y, "ALERTS:", "None", COL_WHITE);
    }

    // Kp
    float kp = screenSolarGetKp();
    if (kp > 0) {
        const char *cond;
        if (kp < 3) cond = "Quiet";
        else if (kp < 4) cond = "Unsettled";
        else if (kp < 5) cond = "Active";
        else if (kp < 6) cond = "G1 Minor";
        else if (kp < 7) cond = "G2 Moderate";
        else if (kp < 8) cond = "G3 Strong";
        else if (kp < 9) cond = "G4 Severe";
        else cond = "G5 Extreme";
        uint16_t kc = (kp >= 5) ? COL_RED : (kp >= 3) ? COL_AMBER : COL_WHITE;
        snprintf(buf, sizeof(buf), "Kp %.1f  %s", kp, cond);
        drawThreatRow(tft, y, "SOLAR:", buf, kc);
    } else {
        drawThreatRow(tft, y, "SOLAR:", "No data", COL_DIM);
    }

    // Fires
    int fireN = screenFiresGetCount();
    if (fireN > 0) {
        const char *ft = screenFiresGetFirstTitle();
        char fs[48];
        int len = strlen(ft);
        if (len > 42) {
            memcpy(fs, ft, 42); fs[42] = '.'; fs[43] = '.'; fs[44] = '\0';
        } else {
            snprintf(fs, sizeof(fs), "%s", ft);
        }
        snprintf(buf, sizeof(buf), "%d open", fireN);
        drawThreatRow(tft, y, "FIRES:", buf, COL_AMBER);
        // Show newest fire title below
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(12, y);
        tft.print(fs);
        y += 14;
    } else {
        drawThreatRow(tft, y, "FIRES:", "No data", COL_DIM);
    }

    // Quakes
    int quakeN = screenUsgsGetCount();
    if (quakeN > 0) {
        float maxMag = screenUsgsGetMaxMag();
        const char *place = screenUsgsGetMaxPlace();
        uint16_t mc = (maxMag >= 7.0f) ? COL_RED : (maxMag >= 5.0f) ? COL_AMBER : COL_WHITE;
        snprintf(buf, sizeof(buf), "M%.1f  %s", maxMag, place);
        drawThreatRow(tft, y, "QUAKES:", buf, mc);
    } else {
        drawThreatRow(tft, y, "QUAKES:", "No data", COL_DIM);
    }

    y += 4;

    // ── CONDITIONS ───────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, y);
    tft.print("CONDITIONS");
    tft.drawFastHLine(8, y + 10, SCREEN_W - 16, g_themeColor);
    y += 16;

    if (g_current.valid) {
        snprintf(buf, sizeof(buf), "%d%sF  %s",
                 (int)roundf(g_current.temp),
                 g_invert ? "" : "",
                 wmoDescription(g_current.weather_code));
        drawCondRow(tft, y, "Temp:", buf);

        snprintf(buf, sizeof(buf), "%dF / %dF",
                 (int)roundf(g_current.today_max),
                 (int)roundf(g_current.today_min));
        drawCondRow(tft, y, "Hi/Lo:", buf);

        snprintf(buf, sizeof(buf), "%d%%", g_current.humidity);
        drawCondRow(tft, y, "Humidity:", buf);

        snprintf(buf, sizeof(buf), "%.0f mph  %s",
                 g_current.wind_speed, windCardinal(g_current.wind_dir));
        drawCondRow(tft, y, "Wind:", buf);

        snprintf(buf, sizeof(buf), "%.1f", g_current.uv_index);
        drawCondRow(tft, y, "UV:", buf);
    } else {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(12, y);
        tft.print("Weather data not loaded");
        y += 14;
    }

    // Latest news headline
    const char *newsHead = screenNewsGetFirstTitle();
    if (newsHead && newsHead[0]) {
        y += 4;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(8, y);
        tft.print("NEWS:");
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(56, y);
        char nb[48];
        int nl = strlen(newsHead);
        if (nl > 44) { memcpy(nb, newsHead, 44); nb[44] = '.'; nb[45] = '.'; nb[46] = '\0'; }
        else { snprintf(nb, sizeof(nb), "%s", newsHead); }
        tft.print(nb);
    }
}

void screenSitrepTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    // No fetch — data updates when individual screens refresh
}
