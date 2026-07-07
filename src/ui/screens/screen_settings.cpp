#include "screen_settings.h"
#include "../theme.h"
#include "../theme_color.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../config/nvs_config.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include "../../modules/brightness.h"
#include "../../touch/touch.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <cstdio>
#include <cstring>

// ── Layout constants ──────────────────────────────────────────────────────────
// Section 1 — Sleep Timer (replaces old WiFi/IP/LOC/BAT info)
#define SLP_LABEL_Y   (CONTENT_Y + 8)    // 31
#define SLP_BTN_Y0    (SLP_LABEL_Y + 10) // 37
#define SLP_BTN_H     13
#define SLP_BTN_W     28
#define SLP_COUNT      5
#define SLP_GAP        2

// Schedule row — 3 buttons under sleep timer
#define SCHED_BTN_Y    (SLP_BTN_Y0 + SLP_BTN_H + 1)   // 51
#define SCHED_PAD      3    // px padding each side of text inside button
#define SLEEP_W        64   // unchanged per user request
#define SCHED_GAP      2
#define SCHED_COUNT    5    // sleep hour options: 8PM-12AM
#define WAKE_COUNT     5    // wake hour options: 5AM-9AM

// Dynamic button geometry (computed during draw, used by both draw & tap)
static int s_schedX1 = 0, s_schedW = 0;
static int s_schedX2 = 0, s_sleepW = 0;
static int s_schedX3 = 0, s_wakeW  = 0;

// Right column — 2×2 grid, same height as brightness buttons
#define BTN_X1       180
#define BTN_X2       (BTN_X1 + BTN_SQ_W + 2)   // 247
#define BTN_SQ_W     65
#define BTN_SQ_H     18   // matches BRI_BTN_H

#define EINK_X       BTN_X1                    // 180
#define EINK_Y       SLP_LABEL_Y               // 27
#define PWR_X        BTN_X2                    // 247
#define PWR_Y        SLP_LABEL_Y               // 27
#define REFRESH_X    BTN_X1                    // 180
#define REFRESH_Y    (EINK_Y + BTN_SQ_H + 2)   // 47
#define LOC_X        BTN_X2                    // 247
#define LOC_Y        (EINK_Y + BTN_SQ_H + 2)   // 47

// Divider between info and controls
#define DIV1_Y       79

// Section 2 — Brightness + Rotate + Page checkboxes (middle)
#define SEC2_X       8
#define SEC2_W       (SCREEN_W - 2 * SEC2_X)   // 304

#define BRI_LABEL_Y  (DIV1_Y + 4)               // 83
#define BRI_BTN_Y0   (BRI_LABEL_Y + 8)          // 91
#define BRI_BTN_H    18
#define BRI_BTN_W    ((SEC2_W - (BRI_LEVELS - 1) * 3) / BRI_LEVELS)  // 48

#define ROT_LABEL_Y  (BRI_BTN_Y0 + BRI_BTN_H + 7)   // 116
#define ROT_BTN_Y0   (ROT_LABEL_Y + 8)               // 124
#define ROT_BTN_H    18
#define ROT_BTN_COUNT 5
#define ROT_BTN_W    ((SEC2_W - (ROT_BTN_COUNT - 1) * 3) / ROT_BTN_COUNT)  // 58

#define PAGE_LABEL_Y (ROT_BTN_Y0 + ROT_BTN_H + 8)    // 150
#define PAGE_BTN_Y0  (PAGE_LABEL_Y + 8)               // 158
#define PAGE_BTN_H   18
#define PAGE_GAP     2
#define PAGE_COUNT   11
#define PAGE_BTN_W   ((SEC2_W - (PAGE_COUNT - 1) * PAGE_GAP) / PAGE_COUNT)  // 28
#define PAGE_EXTRA   (SEC2_W - (PAGE_COUNT * PAGE_BTN_W + (PAGE_COUNT - 1) * PAGE_GAP))  // px remainder
#define PAGE_SPLIT   (PAGE_EXTRA / 2)      // split extra between last 2 boxes (both 2-digit)

// Section 3 — Theme Color (bottom)
#define THEME_LABEL_Y 182   // moved up 3px from 185
#define SWATCH_Y0     192   // moved up 3px from 195
#define SWATCH_H      20
#define SWATCH_W      ((SEC2_W - (THEME_COUNT - 1) * 2) / THEME_COUNT)  // 32
#define SWATCH_PAD    2

// ── Sleep constants ──────────────────────────────────────────────────────────
static const char *s_slpLabels[SLP_COUNT] = {"OFF","15s","30s","1m","5m"};
static const uint32_t s_slpSecs[SLP_COUNT] = {0, 15, 30, 60, 300};

static int s_slpTimer = -1;   // cached NVS value

static int slpCacheLoad() {
    if (s_slpTimer < 0) {
        s_slpTimer = nvsGetInt("slp_timer", 0);
        if (s_slpTimer < 0 || s_slpTimer >= SLP_COUNT) s_slpTimer = 0;
    }
    return s_slpTimer;
}

static void slpCacheSave() {
    nvsPutInt("slp_timer", s_slpTimer);
}

// ── Schedule state ──────────────────────────────────────────────────────────
static const int   s_sleepHours[SCHED_COUNT] = {20, 21, 22, 23, 0};
static const char *s_sleepHourLabels[SCHED_COUNT] = {"8PM","9PM","10PM","11PM","12AM"};
static const int   s_wakeHours[WAKE_COUNT]   = {5, 6, 7, 8, 9};
static const char *s_wakeHourLabels[WAKE_COUNT]   = {"5AM","6AM","7AM","8AM","9AM"};

static bool s_schedEnabled   = false;
static int  s_sleepHourIdx   = 2;    // default: 10PM
static int  s_wakeHourIdx    = 2;    // default: 7AM
static bool s_schedLoaded    = false;

static void schedLoad() {
    if (s_schedLoaded) return;
    s_schedEnabled = nvsGetInt("sched_en", 0) != 0;
    s_sleepHourIdx = nvsGetInt("sched_slp", 2);
    if (s_sleepHourIdx < 0 || s_sleepHourIdx >= SCHED_COUNT) s_sleepHourIdx = 2;
    s_wakeHourIdx  = nvsGetInt("sched_wke", 2);
    if (s_wakeHourIdx  < 0 || s_wakeHourIdx  >= WAKE_COUNT)  s_wakeHourIdx  = 2;
    s_schedLoaded = true;
}

static void schedSave() {
    nvsPutInt("sched_en", s_schedEnabled ? 1 : 0);
    nvsPutInt("sched_slp", s_sleepHourIdx);
    nvsPutInt("sched_wke", s_wakeHourIdx);
}

static const char *s_briLabels[BRI_LEVELS] = {"AUTO","DIM","LOW","MED","HIGH","MAX"};
static bool s_refreshRequested = false;
static bool s_pwrConfirm = false;
static unsigned long s_pwrConfirmMs = 0;

// ── Auto-rotate state ─────────────────────────────────────────────────────────
static const uint32_t s_rotMs[]     = { 0, 5000, 10000, 30000, 60000 };
static const char    *s_rotLabels[] = { "OFF", "5s", "10s", "30s", "1m" };
static int   s_autoRotSel    = 0;
static bool  s_autoRotLoaded = false;

static void autoRotLoad() {
    if (s_autoRotLoaded) return;
    s_autoRotSel = nvsGetInt("arot_sel", 0);
    if (s_autoRotSel < 0 || s_autoRotSel >= ROT_BTN_COUNT) s_autoRotSel = 0;
    s_autoRotLoaded = true;
}

// ── Page rotation mask ────────────────────────────────────────────────────────
// Bit 0 = NOW, ..., bit 9 = PLANNER
static uint16_t s_pageMask  = 0x3FF;  // 10 pages (0-9)
static bool     s_pageLoaded = false;

static void pageMaskLoad() {
    if (s_pageLoaded) return;
    int v2 = nvsGetInt("page_mask_v2", -1);
    if (v2 < 0) {
        uint16_t old = (uint16_t)nvsGetInt("page_mask", 0x3FF);
        s_pageMask = (old & 0x000F) | 0x0010 | ((old & 0x03F0) << 1);
        nvsPutInt("page_mask_v2", s_pageMask);
    } else {
        s_pageMask = (uint16_t)v2;
    }
    if (s_pageMask == 0) s_pageMask = 0x3FF;
    s_pageLoaded = true;
}

static void pageMaskSave() {
    nvsPutInt("page_mask_v2", s_pageMask);
}

// ── Favorite mask ────────────────────────────────────────────────────────────
static uint16_t s_favMask  = 0x0000;
static bool     s_favLoaded = false;

static void favMaskLoad() {
    if (s_favLoaded) return;
    s_favMask = (uint16_t)nvsGetInt("fav_mask", 0x0000);
    s_favLoaded = true;
}

static void favMaskSave() {
    nvsPutInt("fav_mask", s_favMask);
}

// ── Double-tap state for favorite toggle ─────────────────────────────────────
static int          s_lastTapPage = -1;
static unsigned long s_lastTapMs   = 0;

// ── Rotation alternation state ───────────────────────────────────────────────
static int s_lastFavPage = -1;   // last favorite shown in rotation
static int s_lastRegPage = -1;   // last regular shown in rotation

bool screenSettingsGetPageEnabled(int page) {
    pageMaskLoad();
    if (page < 0 || page >= PAGE_COUNT) return false;
    return (s_pageMask >> page) & 1;
}

bool screenSettingsIsFavorite(int page) {
    favMaskLoad();
    if (page < 0 || page >= PAGE_COUNT) return false;
    return (s_favMask >> page) & 1;
}

int screenSettingsGetNextRotatePage(int current) {
    pageMaskLoad();
    favMaskLoad();

    // ── No favorites: original round-robin behavior ──────────────────────
    if (s_favMask == 0) {
        if (s_pageMask == 0) return -1;
        for (int i = 1; i <= PAGE_COUNT; i++) {
            int p = (current + i) % PAGE_COUNT;
            if ((s_pageMask >> p) & 1) return p;
        }
        return -1;
    }

    // ── Favorites active: alternate fav / regular ────────────────────────
    bool currentIsFav = (s_favMask >> current) & 1;

    if (currentIsFav) {
        // Just showed a favorite → find next regular
        int start = (s_lastRegPage >= 0) ? s_lastRegPage : current;
        for (int i = 1; i <= PAGE_COUNT; i++) {
            int p = (start + i) % PAGE_COUNT;
            if (((s_pageMask >> p) & 1) && !((s_favMask >> p) & 1)) {
                s_lastRegPage = p;
                return p;
            }
        }
        // No regulars available — fall through to favorite round-robin
    }

    // Show a favorite next (either current was regular, or no regulars exist)
    {
        int start = (s_lastFavPage >= 0) ? s_lastFavPage : current;
        for (int i = 1; i <= PAGE_COUNT; i++) {
            int p = (start + i) % PAGE_COUNT;
            if ((s_favMask >> p) & 1) {
                s_lastFavPage = p;
                return p;
            }
        }
    }

    // Fallback (shouldn't be reached if favMask != 0, but be safe)
    return -1;
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void screenSettingsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SETTINGS", timeStr, wifiOk);
    drawBottombar(tft, "", 11, 12);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    slpCacheLoad();

    // ── Section 1: Sleep Timer ──────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, SLP_LABEL_Y);
    tft.print("SLEEP TIMER");

    for (int i = 0; i < SLP_COUNT; i++) {
        int bx = SEC2_X + i * (SLP_BTN_W + SLP_GAP);
        int by = SLP_BTN_Y0;
        tft.fillRect(bx, by, SLP_BTN_W, SLP_BTN_H, COL_INPUTBG);
        if (i == s_slpTimer) {
            tft.drawRect(bx - 1, by - 1, SLP_BTN_W + 2, SLP_BTN_H + 2, g_themeColor);
            tft.setTextColor(g_themeColor, COL_INPUTBG);
        } else {
            tft.drawRect(bx, by, SLP_BTN_W, SLP_BTN_H, COL_DIM);
            tft.setTextColor(COL_DIM, COL_INPUTBG);
        }
        int tw = tft.textWidth(s_slpLabels[i]);
        tft.setCursor(bx + (SLP_BTN_W - tw) / 2, by + (SLP_BTN_H - 8) / 2);
        tft.print(s_slpLabels[i]);
    }

    // ── Schedule row ───────────────────────────────────────────────────────────
    schedLoad();
    {
        int by = SCHED_BTN_Y;

        // Measure text and compute tight-fit button widths
        int schedTw = tft.textWidth("SCHED");

        char sleepBuf[12];
        snprintf(sleepBuf, sizeof(sleepBuf), "SLEEP %s", s_sleepHourLabels[s_sleepHourIdx]);
        int sleepTw = tft.textWidth(sleepBuf);

        char wakeBuf[12];
        snprintf(wakeBuf, sizeof(wakeBuf), "WAKE %s", s_wakeHourLabels[s_wakeHourIdx]);
        int wakeTw = tft.textWidth(wakeBuf);

        int schedW = schedTw + SCHED_PAD * 2;
        int sleepW = SLEEP_W;   // fixed width, leave as is
        int wakeW  = wakeTw + SCHED_PAD * 2;

        int x1 = SEC2_X;
        int x2 = x1 + schedW + SCHED_GAP;
        int x3 = x2 + sleepW + SCHED_GAP;

        // Save for tap hit-testing
        s_schedX1 = x1; s_schedW = schedW;
        s_schedX2 = x2; s_sleepW = sleepW;
        s_schedX3 = x3; s_wakeW  = wakeW;

        // Button 1: Schedule toggle (tight-fit, themed like other buttons)
        {
            tft.fillRect(x1, by, schedW, SLP_BTN_H, COL_INPUTBG);
            if (s_schedEnabled) {
                tft.drawRect(x1 - 1, by - 1, schedW + 2, SLP_BTN_H + 2, g_themeColor);
                tft.setTextColor(g_themeColor, COL_INPUTBG);
            } else {
                tft.drawRect(x1, by, schedW, SLP_BTN_H, COL_DIM);
                tft.setTextColor(COL_DIM, COL_INPUTBG);
            }
            tft.setCursor(x1 + (schedW - schedTw) / 2, by + (SLP_BTN_H - 8) / 2);
            tft.print("SCHED");
        }
        // Button 2: Sleep time (unchanged)
        {
            tft.fillRect(x2, by, sleepW, SLP_BTN_H, COL_INPUTBG);
            if (s_schedEnabled) {
                tft.drawRect(x2 - 1, by - 1, sleepW + 2, SLP_BTN_H + 2, g_themeColor);
                tft.setTextColor(g_themeColor, COL_INPUTBG);
            } else {
                tft.drawRect(x2, by, sleepW, SLP_BTN_H, COL_DIM);
                tft.setTextColor(COL_DIM, COL_INPUTBG);
            }
            tft.setCursor(x2 + (sleepW - sleepTw) / 2, by + (SLP_BTN_H - 8) / 2);
            tft.print(sleepBuf);
        }
        // Button 3: Wake time (tight-fit)
        {
            tft.fillRect(x3, by, wakeW, SLP_BTN_H, COL_INPUTBG);
            if (s_schedEnabled) {
                tft.drawRect(x3 - 1, by - 1, wakeW + 2, SLP_BTN_H + 2, g_themeColor);
                tft.setTextColor(g_themeColor, COL_INPUTBG);
            } else {
                tft.drawRect(x3, by, wakeW, SLP_BTN_H, COL_DIM);
                tft.setTextColor(COL_DIM, COL_INPUTBG);
            }
            tft.setCursor(x3 + (wakeW - wakeTw) / 2, by + (SLP_BTN_H - 8) / 2);
            tft.print(wakeBuf);
        }
    }

    // ── Right column: 2×2 grid (same style as brightness buttons) ─────────────
    // Top-left: E-Ink toggle
    {
        tft.fillRect(EINK_X, EINK_Y, BTN_SQ_W, BTN_SQ_H, COL_INPUTBG);
        if (invertGet()) {
            tft.drawRect(EINK_X - 1, EINK_Y - 1, BTN_SQ_W + 2, BTN_SQ_H + 2, COL_WHITE);
            tft.drawRect(EINK_X - 2, EINK_Y - 2, BTN_SQ_W + 4, BTN_SQ_H + 4, COL_WHITE);
            tft.setTextColor(COL_BG, COL_INPUTBG);
        } else {
            tft.drawRect(EINK_X, EINK_Y, BTN_SQ_W, BTN_SQ_H, g_themeColor);
            tft.setTextColor(g_themeColor, COL_INPUTBG);
        }
        tft.setTextFont(FONT_SM);
        char einkLabel[12];
        snprintf(einkLabel, sizeof(einkLabel), "E-INK %s", invertGet() ? "ON" : "OFF");
        int elw = tft.textWidth(einkLabel);
        tft.setCursor(EINK_X + (BTN_SQ_W - elw) / 2, EINK_Y + (BTN_SQ_H - 8) / 2);
        tft.print(einkLabel);
    }

    // Top-right: Power Off
    {
        tft.fillRect(PWR_X, PWR_Y, BTN_SQ_W, BTN_SQ_H, COL_INPUTBG);
        tft.drawRect(PWR_X, PWR_Y, BTN_SQ_W, BTN_SQ_H, COL_RED);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_RED, COL_INPUTBG);
        const char *pwrLabel = s_pwrConfirm ? "SURE?" : "PWR OFF";
        int plw = tft.textWidth(pwrLabel);
        tft.setCursor(PWR_X + (BTN_SQ_W - plw) / 2, PWR_Y + (BTN_SQ_H - 8) / 2);
        tft.print(pwrLabel);
    }

    // Bottom-left: Refresh Weather
    {
        tft.fillRect(REFRESH_X, REFRESH_Y, BTN_SQ_W, BTN_SQ_H, COL_INPUTBG);
        tft.drawRect(REFRESH_X, REFRESH_Y, BTN_SQ_W, BTN_SQ_H, g_themeColor);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_INPUTBG);
        int rlw = tft.textWidth("REFRESH");
        tft.setCursor(REFRESH_X + (BTN_SQ_W - rlw) / 2, REFRESH_Y + (BTN_SQ_H - 8) / 2);
        tft.print("REFRESH");
    }

    // Bottom-right: Update Location
    {
        tft.fillRect(LOC_X, LOC_Y, BTN_SQ_W, BTN_SQ_H, COL_INPUTBG);
        tft.drawRect(LOC_X, LOC_Y, BTN_SQ_W, BTN_SQ_H, COL_AMBER);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_AMBER, COL_INPUTBG);
        int llw = tft.textWidth("LOCATION");
        tft.setCursor(LOC_X + (BTN_SQ_W - llw) / 2, LOC_Y + (BTN_SQ_H - 8) / 2);
        tft.print("LOCATION");
    }

    // Divider 1
    tft.drawFastHLine(0, DIV1_Y, SCREEN_W, COL_DIM);

    // ── Section 2: Brightness ─────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, BRI_LABEL_Y);
    tft.print("BRIGHTNESS");

    int activeBri = brightnessGetLevel();
    for (int i = 0; i < BRI_LEVELS; i++) {
        int bx = SEC2_X + i * (BRI_BTN_W + 3);
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

    // ── Section 2: Rotate ─────────────────────────────────────────────────────
    autoRotLoad();
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, ROT_LABEL_Y);
    tft.print("ROTATE");

    for (int i = 0; i < ROT_BTN_COUNT; i++) {
        int bx = SEC2_X + i * (ROT_BTN_W + 3);
        int by = ROT_BTN_Y0;
        tft.fillRect(bx, by, ROT_BTN_W, ROT_BTN_H, COL_INPUTBG);
        if (i == s_autoRotSel) {
            tft.drawRect(bx - 1, by - 1, ROT_BTN_W + 2, ROT_BTN_H + 2, g_themeColor);
            tft.setTextColor(g_themeColor, COL_INPUTBG);
        } else {
            tft.drawRect(bx, by, ROT_BTN_W, ROT_BTN_H, COL_DIM);
            tft.setTextColor(COL_DIM, COL_INPUTBG);
        }
        int tw = tft.textWidth(s_rotLabels[i]);
        tft.setCursor(bx + (ROT_BTN_W - tw) / 2, by + (ROT_BTN_H - 8) / 2);
        tft.print(s_rotLabels[i]);
    }

    // ── Section 2: Page Rotation checkboxes (single row) ──────────────────────
    pageMaskLoad();
    favMaskLoad();
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, PAGE_LABEL_Y);
    tft.print("PAGE ROTATION");

    for (int i = 0; i < PAGE_COUNT; i++) {
        int bx  = SEC2_X + i * (PAGE_BTN_W + PAGE_GAP);
        int by  = PAGE_BTN_Y0;
        int bw  = PAGE_BTN_W;
        if (i == PAGE_COUNT - 1)      bw += PAGE_EXTRA - PAGE_SPLIT;  // last
        else if (i == PAGE_COUNT - 2) bw += PAGE_SPLIT;               // 2nd-to-last
        bool enabled = (s_pageMask >> i) & 1;
        bool favorite = enabled && ((s_favMask >> i) & 1);

        tft.fillRect(bx, by, bw, PAGE_BTN_H, COL_INPUTBG);
        tft.drawRect(bx, by, bw, PAGE_BTN_H, enabled ? g_themeColor : COL_DIM);

        const int chkSize = 10;
        int chkX = bx + 3;
        int chkY = by + (PAGE_BTN_H - chkSize) / 2;
        tft.drawRect(chkX, chkY, chkSize, chkSize, enabled ? g_themeColor : COL_DIM);
        if (enabled) {
            uint16_t fill = favorite ? COL_AMBER : g_themeColor;
            tft.fillRect(chkX + 2, chkY + 2, chkSize - 4, chkSize - 4, fill);
        }

        char numBuf[3];
        snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        tft.setTextColor(favorite ? COL_AMBER : (enabled ? COL_WHITE : COL_DIM), COL_INPUTBG);
        // Right-align the number in the box
        int numW = tft.textWidth(numBuf);
        tft.setCursor(bx + bw - numW - 4, by + (PAGE_BTN_H - 8) / 2);
        tft.print(numBuf);
    }

    // ── Section 3: Theme Color (bottom) ───────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, THEME_LABEL_Y);
    tft.print("THEME COLOR");

    int activeTheme = themeColorGetIdx();
    for (int i = 0; i < THEME_COUNT; i++) {
        int sx = SEC2_X + i * (SWATCH_W + SWATCH_PAD);
        tft.fillRect(sx, SWATCH_Y0, SWATCH_W, SWATCH_H, g_themes[i].color);
        if (i == activeTheme)
            tft.drawRect(sx - 2, SWATCH_Y0 - 2, SWATCH_W + 4, SWATCH_H + 4, COL_WHITE);
    }
}

// ── Tap handling ──────────────────────────────────────────────────────────────
bool screenSettingsTap(TFT_eSPI &tft, int16_t tx, int16_t ty) {
    if (s_pwrConfirm && millis() - s_pwrConfirmMs > 5000) {
        s_pwrConfirm = false;
    }

    // E-Ink toggle (top-left)
    if (tx >= EINK_X && tx < EINK_X + BTN_SQ_W &&
        ty >= EINK_Y && ty < EINK_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        invertSet(!invertGet());
        return true;
    }

    // Power Off (top-right) — two-tap confirmation
    if (tx >= PWR_X && tx < PWR_X + BTN_SQ_W &&
        ty >= PWR_Y && ty < PWR_Y + BTN_SQ_H) {
        if (s_pwrConfirm && millis() - s_pwrConfirmMs < 5000) {
            tft.fillScreen(COL_BG);
            tft.setTextFont(FONT_MD);
            tft.setTextColor(g_themeColor, COL_BG);
            tft.setCursor(80, 110);
            tft.print("Shutting down...");
            delay(500);
            // Deep sleep — wake on touch IRQ (GPIO 36, active low)
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
            esp_deep_sleep_start();
        } else {
            s_pwrConfirm = true;
            s_pwrConfirmMs = millis();
        }
        return true;
    }

    // Refresh Weather (bottom-left)
    if (tx >= REFRESH_X && tx < REFRESH_X + BTN_SQ_W &&
        ty >= REFRESH_Y && ty < REFRESH_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        s_refreshRequested = true;
        return true;
    }

    // Update Location (bottom-right)
    if (tx >= LOC_X && tx < LOC_X + BTN_SQ_W &&
        ty >= LOC_Y && ty < LOC_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        s_refreshRequested = true;
        return true;
    }

    // Sleep timer buttons
    slpCacheLoad();
    for (int i = 0; i < SLP_COUNT; i++) {
        int bx = SEC2_X + i * (SLP_BTN_W + SLP_GAP);
        if (tx >= bx && tx < bx + SLP_BTN_W &&
            ty >= SLP_BTN_Y0 && ty < SLP_BTN_Y0 + SLP_BTN_H) {
            s_slpTimer = i;
            slpCacheSave();
            return true;
        }
    }

    // Schedule row buttons
    schedLoad();
    {
        int by = SCHED_BTN_Y;
        // Button 1: SCHED toggle
        if (tx >= s_schedX1 && tx < s_schedX1 + s_schedW &&
            ty >= by && ty < by + SLP_BTN_H) {
            s_schedEnabled = !s_schedEnabled;
            schedSave();
            return true;
        }
        // Button 2: Sleep time cycle
        if (tx >= s_schedX2 && tx < s_schedX2 + s_sleepW &&
            ty >= by && ty < by + SLP_BTN_H) {
            s_sleepHourIdx = (s_sleepHourIdx + 1) % SCHED_COUNT;
            schedSave();
            return true;
        }
        // Button 3: Wake time cycle
        if (tx >= s_schedX3 && tx < s_schedX3 + s_wakeW &&
            ty >= by && ty < by + SLP_BTN_H) {
            s_wakeHourIdx = (s_wakeHourIdx + 1) % WAKE_COUNT;
            schedSave();
            return true;
        }
    }

    // Brightness
    for (int i = 0; i < BRI_LEVELS; i++) {
        int bx = SEC2_X + i * (BRI_BTN_W + 3);
        if (tx >= bx && tx < bx + BRI_BTN_W &&
            ty >= BRI_BTN_Y0 && ty < BRI_BTN_Y0 + BRI_BTN_H) {
            brightnessSetLevel(i);
            return true;
        }
    }

    // Auto-rotate
    autoRotLoad();
    for (int i = 0; i < ROT_BTN_COUNT; i++) {
        int bx = SEC2_X + i * (ROT_BTN_W + 3);
        if (tx >= bx && tx < bx + ROT_BTN_W &&
            ty >= ROT_BTN_Y0 && ty < ROT_BTN_Y0 + ROT_BTN_H) {
            s_autoRotSel = i;
            nvsPutInt("arot_sel", i);
            return true;
        }
    }

    // Page rotation checkboxes
    pageMaskLoad();
    favMaskLoad();
    for (int i = 0; i < PAGE_COUNT; i++) {
        int bx = SEC2_X + i * (PAGE_BTN_W + PAGE_GAP);
        int bw = PAGE_BTN_W;
        if (i == PAGE_COUNT - 1)      bw += PAGE_EXTRA - PAGE_SPLIT;
        else if (i == PAGE_COUNT - 2) bw += PAGE_SPLIT;
        if (tx >= bx && tx < bx + bw &&
            ty >= PAGE_BTN_Y0 && ty < PAGE_BTN_Y0 + PAGE_BTN_H) {

            // Double-tap: same box within 500ms → toggle favorite
            if (s_lastTapPage == i && (millis() - s_lastTapMs) < 500) {
                s_favMask ^= (1 << i);
                favMaskSave();
                s_lastTapPage = -1;   // reset — prevent triple-tap
                return true;
            }

            // Single tap: toggle enable
            s_pageMask ^= (1 << i);
            if (s_pageMask == 0) s_pageMask = (1 << i);
            // Disabling a page also clears its favorite flag
            if (!((s_pageMask >> i) & 1)) {
                s_favMask &= ~(1u << i);
                favMaskSave();
            }
            pageMaskSave();
            s_lastTapPage = i;
            s_lastTapMs   = millis();
            return true;
        }
    }

    // Theme swatches
    for (int i = 0; i < THEME_COUNT; i++) {
        int sx = SEC2_X + i * (SWATCH_W + SWATCH_PAD);
        if (tx >= sx && tx < sx + SWATCH_W &&
            ty >= SWATCH_Y0 && ty < SWATCH_Y0 + SWATCH_H) {
            themeColorSet(i);
            return true;
        }
    }

    return false;
}

bool screenSettingsRefreshTapped() {
    if (s_refreshRequested) {
        s_refreshRequested = false;
        return true;
    }
    return false;
}

int screenSettingsGetSleepTimerSecs() {
    int idx = nvsGetInt("slp_timer", 0);
    if (idx < 0 || idx >= SLP_COUNT) return 0;
    return (int)s_slpSecs[idx];
}

bool screenSettingsGetScheduleEnabled() {
    schedLoad();
    return s_schedEnabled;
}

int screenSettingsGetSleepHour() {
    schedLoad();
    return s_sleepHours[s_sleepHourIdx];   // 0-23
}

int screenSettingsGetWakeHour() {
    schedLoad();
    return s_wakeHours[s_wakeHourIdx];     // 0-23
}

bool screenSettingsGetAutoRotate() {
    autoRotLoad();
    return s_autoRotSel > 0;
}

uint32_t screenSettingsGetAutoRotateMs() {
    autoRotLoad();
    return s_rotMs[s_autoRotSel];
}
