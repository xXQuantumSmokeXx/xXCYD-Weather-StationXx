#include "widgets.h"
#include "theme.h"
#include "../config/config.h"
#include "../modules/brightness.h"
#include <cstring>
#include <cstdio>

// Arm lengths for corner bracket ticks
#define TK_H  10   // horizontal arm
#define TK_V   6   // vertical arm

void drawTopbar(TFT_eSPI &tft, const char *city, const char *screenLabel, const char *timeStr, bool wifiOk) {
    tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_BG);

    // ── Double border: dim line above, themed line at very bottom ─────────
    tft.drawFastHLine(0, TOPBAR_H - 2, SCREEN_W, COL_DIM);
    tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, g_themeColor);

    // ── Corner bracket ticks — top-left and top-right ─────────────────────
    tft.drawFastHLine(0,              0, TK_H, g_themeColor);
    tft.drawFastVLine(0,              0, TK_V, g_themeColor);
    tft.drawFastHLine(SCREEN_W - TK_H, 0, TK_H, g_themeColor);
    tft.drawFastVLine(SCREEN_W - 1,    0, TK_V, g_themeColor);

    // ── Screen label — centered, themed, with accent squares at border ────
    if (screenLabel && screenLabel[0]) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        int lw = tft.textWidth(screenLabel);
        int lx = (SCREEN_W - lw) / 2;
        tft.setCursor(lx, 3);
        tft.print(screenLabel);

        // Small filled squares flanking the label on the border line
        tft.fillRect(lx - 6,      TOPBAR_H - 5, 3, 4, g_themeColor);
        tft.fillRect(lx + lw + 3, TOPBAR_H - 5, 3, 4, g_themeColor);
    }

    // ── City — left, shifted inside corner bracket ────────────────────────
    if (city && city[0]) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(TK_H + 3, 3);
        tft.print(city);
    }

    // ── Time — right, shifted inside corner bracket ───────────────────────
    tft.setTextFont(FONT_MD);
    int tw = tft.textWidth(timeStr);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(SCREEN_W - tw - TK_H - 3, 3);
    tft.print(timeStr);
}

void drawBottombar(TFT_eSPI &tft, const char *label, int activeScreen, int totalScreens) {
    int y0 = SCREEN_H - BOTBAR_H;
    tft.fillRect(0, y0, SCREEN_W, BOTBAR_H, COL_BG);

    // ── Double border: themed line at top, dim line just below ────────────
    tft.drawFastHLine(0, y0,     SCREEN_W, g_themeColor);
    tft.drawFastHLine(0, y0 + 1, SCREEN_W, COL_DIM);

    // ── Corner bracket ticks — bottom-left and bottom-right ───────────────
    tft.drawFastHLine(0,              SCREEN_H - 1, TK_H, g_themeColor);
    tft.drawFastVLine(0,              SCREEN_H - TK_V, TK_V, g_themeColor);
    tft.drawFastHLine(SCREEN_W - TK_H, SCREEN_H - 1, TK_H, g_themeColor);
    tft.drawFastVLine(SCREEN_W - 1,    SCREEN_H - TK_V, TK_V, g_themeColor);

    int my = y0 + BOTBAR_H / 2;

    // ── Navigation arrows — symmetric spacing inside corner brackets ─────
    tft.setTextFont(FONT_MD);
    int arrowW = tft.textWidth(">");   // same width as "<" in this font

    uint16_t lCol = (activeScreen > 0) ? g_themeColor : COL_DIM;
    tft.setTextColor(lCol, COL_BG);
    tft.setCursor(TK_H + 2, my - 8);
    tft.print("<");

    int rarrowX = SCREEN_W - TK_H - 2 - arrowW;   // mirrors left arrow distance
    uint16_t rCol = (activeScreen < totalScreens - 1) ? g_themeColor : COL_DIM;
    tft.setTextColor(rCol, COL_BG);
    tft.setCursor(rarrowX, my - 8);
    tft.print(">");

    // ── Battery % — themed, just left of right arrow ──────────────────────
    int batt = batteryPct();
    if (batt >= 0) {
        char bbuf[8];
        snprintf(bbuf, sizeof(bbuf), "%d%%", batt);
        tft.setTextFont(FONT_SM);
        int bw = tft.textWidth(bbuf);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(rarrowX - bw - 4, my - 4);
        tft.print(bbuf);
    }

    // ── Page indicator dots (shown when no label is given) ─────────────────
    if (!label || !label[0]) {
        const int DS = 4, DG = 6;
        int total = totalScreens * DS + (totalScreens - 1) * DG;
        int dx = (SCREEN_W - total) / 2;
        int dy = my - DS / 2;
        for (int i = 0; i < totalScreens; i++) {
            if (i == activeScreen)
                tft.fillRect(dx, dy, DS, DS, g_themeColor);
            else
                tft.drawRect(dx, dy, DS, DS, COL_DIM);
            dx += DS + DG;
        }
    }

    if (label && label[0]) {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        int lw = tft.textWidth(label);
        tft.setCursor((SCREEN_W - lw) / 2, my - 4);
        tft.print(label);
    }
}

void drawDivider(TFT_eSPI &tft, int y, uint16_t col) {
    if (col == 0) col = COL_DIM;
    tft.drawFastHLine(0, y, SCREEN_W, col);
}

void drawStat(TFT_eSPI &tft, int x, int y, const char *label, const char *value) {
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(x, y);
    tft.print(label);
    tft.print(": ");
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.print(value);
}

void drawPrecipChip(TFT_eSPI &tft, int x, int y, int pct) {
    if (pct < 10) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_RAIN, COL_BG);
    tft.setCursor(x, y);
    tft.print(buf);
}
