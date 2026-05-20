#include "screen_settings.h"
#include "../theme.h"
#include "../theme_color.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include "../../modules/brightness.h"
#include <WiFi.h>
#include <cstdio>
#include <cstring>

// ── Theme swatches ────────────────────────────────────────────────────────────
#define SWATCH_COLS   5
#define SWATCH_ROWS   2
#define SWATCH_W     40
#define SWATCH_H     24
#define SWATCH_X0    10
#define SWATCH_Y0    (CONTENT_Y + 14)
#define SWATCH_PAD    3

// ── Brightness buttons ────────────────────────────────────────────────────────
#define BRI_BTN_W    ((SCREEN_W - 2*SWATCH_X0 - (BRI_LEVELS-1)*3) / BRI_LEVELS)  // 47px
#define BRI_BTN_H    20
#define BRI_BTN_Y0   (SWATCH_Y0 + SWATCH_ROWS*(SWATCH_H+SWATCH_PAD) + 16)

// ── Action buttons ────────────────────────────────────────────────────────────
#define BTN_X   8
#define BTN_Y   (BRI_BTN_Y0 + BRI_BTN_H + 10)
#define BTN_W   120
#define BTN_H   22

static const char *s_briLabels[BRI_LEVELS] = {"AUTO","DIM","LOW","MED","HIGH","MAX"};
static bool s_refreshRequested = false;

void screenSettingsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SETTINGS", timeStr, wifiOk);
    drawBottombar(tft, "", 3, 4);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    // ── Theme section ─────────────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SWATCH_X0, CONTENT_Y + 4);
    tft.print("THEME COLOR");

    int activeTheme = themeColorGetIdx();
    for (int i = 0; i < THEME_COUNT; i++) {
        int col = i % SWATCH_COLS;
        int row = i / SWATCH_COLS;
        int sx  = SWATCH_X0 + col * (SWATCH_W + SWATCH_PAD);
        int sy  = SWATCH_Y0 + row * (SWATCH_H + SWATCH_PAD);

        tft.fillRect(sx, sy, SWATCH_W, SWATCH_H, g_themes[i].color);
        if (i == activeTheme)
            tft.drawRect(sx - 2, sy - 2, SWATCH_W + 4, SWATCH_H + 4, COL_WHITE);

        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_BG, g_themes[i].color);
        int tw = tft.textWidth(g_themes[i].name);
        tft.setCursor(sx + (SWATCH_W - tw) / 2, sy + SWATCH_H / 2 - 4);
        tft.print(g_themes[i].name);
    }

    // ── Brightness section ────────────────────────────────────────────────────
    int briLabelY = SWATCH_Y0 + SWATCH_ROWS * (SWATCH_H + SWATCH_PAD) + 4;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SWATCH_X0, briLabelY);
    tft.print("BRIGHTNESS");

    int activeBri = brightnessGetLevel();
    for (int i = 0; i < BRI_LEVELS; i++) {
        int bx = SWATCH_X0 + i * (BRI_BTN_W + 3);
        int by = BRI_BTN_Y0;

        tft.fillRect(bx, by, BRI_BTN_W, BRI_BTN_H, COL_INPUTBG);
        if (i == activeBri) {
            tft.drawRect(bx - 1, by - 1, BRI_BTN_W + 2, BRI_BTN_H + 2, g_themeColor);
            tft.setTextColor(g_themeColor, COL_INPUTBG);
        } else {
            tft.drawRect(bx, by, BRI_BTN_W, BRI_BTN_H, COL_DIM);
            tft.setTextColor(COL_DIM, COL_INPUTBG);
        }
        int tw = tft.textWidth(s_briLabels[i]);
        tft.setCursor(bx + (BRI_BTN_W - tw) / 2, by + (BRI_BTN_H - 8) / 2);
        tft.print(s_briLabels[i]);
    }

    // ── Action buttons ────────────────────────────────────────────────────────
    tft.fillRect(BTN_X, BTN_Y, BTN_W, BTN_H, COL_INPUTBG);
    tft.drawRect(BTN_X, BTN_Y, BTN_W, BTN_H, g_themeColor);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_INPUTBG);
    tft.setCursor(BTN_X + 8, BTN_Y + 7);
    tft.print("REFRESH WEATHER");

    int btn2x = BTN_X + BTN_W + 12;
    tft.fillRect(btn2x, BTN_Y, BTN_W, BTN_H, COL_INPUTBG);
    tft.drawRect(btn2x, BTN_Y, BTN_W, BTN_H, COL_AMBER);
    tft.setTextColor(COL_AMBER, COL_INPUTBG);
    tft.setCursor(btn2x + 6, BTN_Y + 7);
    tft.print("UPDATE LOCATION");

    // ── Info section ──────────────────────────────────────────────────────────
    int infoY = BTN_Y + BTN_H + 8;
    const int lineH = 12;
    tft.setTextFont(FONT_SM);

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(8, infoY);
    tft.print("WiFi: ");
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.print(WiFi.SSID().c_str());

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(8, infoY + lineH);
    tft.print("IP:   ");
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.print(WiFi.localIP().toString().c_str());

    if (g_location.valid) {
        char locBuf[48];
        snprintf(locBuf, sizeof(locBuf), "%.3f, %.3f", g_location.lat, g_location.lon);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(8, infoY + lineH * 2);
        tft.print("Loc:  ");
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.print(locBuf);
    }

    // ── Battery ───────────────────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(8, infoY + lineH * 3);
    tft.print("BAT:  ");
    int batLabelW = tft.textWidth("BAT:  ");
    int bpct = batteryPct();
    if (bpct >= 0) {
        const int barX = 8 + batLabelW;
        const int barW = 90;
        const int barH = 7;
        const int barY = infoY + lineH * 3 + (8 - barH) / 2;
        uint16_t barCol = (bpct < 20) ? COL_AMBER : g_themeColor;
        tft.drawRect(barX, barY, barW, barH, COL_DIM);
        int fill = (barW - 2) * bpct / 100;
        if (fill > 0) tft.fillRect(barX + 1, barY + 1, fill, barH - 2, barCol);
        // Battery nub (positive terminal)
        tft.fillRect(barX + barW, barY + 2, 3, barH - 4, COL_DIM);
        char batBuf[8];
        snprintf(batBuf, sizeof(batBuf), " %d%%", bpct);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(barX + barW + 5, infoY + lineH * 3);
        tft.print(batBuf);
    } else {
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(8 + batLabelW, infoY + lineH * 3);
        tft.print("N/A (no battery)");
    }
}

bool screenSettingsTap(TFT_eSPI &tft, int16_t tx, int16_t ty) {
    // Theme swatches
    for (int i = 0; i < THEME_COUNT; i++) {
        int col = i % SWATCH_COLS;
        int row = i / SWATCH_COLS;
        int sx  = SWATCH_X0 + col * (SWATCH_W + SWATCH_PAD);
        int sy  = SWATCH_Y0 + row * (SWATCH_H + SWATCH_PAD);
        if (tx >= sx && tx < sx + SWATCH_W && ty >= sy && ty < sy + SWATCH_H) {
            themeColorSet(i);
            return true;
        }
    }

    // Brightness buttons
    for (int i = 0; i < BRI_LEVELS; i++) {
        int bx = SWATCH_X0 + i * (BRI_BTN_W + 3);
        int by = BRI_BTN_Y0;
        if (tx >= bx && tx < bx + BRI_BTN_W && ty >= by && ty < by + BRI_BTN_H) {
            brightnessSetLevel(i);
            return true;
        }
    }

    // Refresh button
    if (tx >= BTN_X && tx < BTN_X + BTN_W && ty >= BTN_Y && ty < BTN_Y + BTN_H)
        s_refreshRequested = true;

    // Location button
    int btn2x = BTN_X + BTN_W + 12;
    if (tx >= btn2x && tx < btn2x + BTN_W && ty >= BTN_Y && ty < BTN_Y + BTN_H)
        s_refreshRequested = true;

    return false;
}

bool screenSettingsRefreshTapped() {
    if (s_refreshRequested) {
        s_refreshRequested = false;
        return true;
    }
    return false;
}
