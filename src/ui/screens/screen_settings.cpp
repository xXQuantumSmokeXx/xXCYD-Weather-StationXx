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
#define SLP_LABEL_Y   (CONTENT_Y + 4)      // 27
#define SLP_BTN_Y0    (SLP_LABEL_Y + 10)   // 37
#define SLP_BTN_H     13
#define SLP_COUNT      7
#define SLP_GAP        2
#define SLP_BTN_W     ((SEC2_W - (SLP_COUNT - 1) * SLP_GAP) / SLP_COUNT)  // ~43px

#define SCHED_X        8
#define SCHED_LABEL_Y  (SLP_BTN_Y0 + SLP_BTN_H + 3)  // 53
#define SCHED_CHK_X    8
#define SCHED_CHK_Y    (SCHED_LABEL_Y + 10)           // 63
#define SCHED_CHK_SZ   11

#define SLP_TIME_LABEL_X 30
#define SLP_TIME_VAL_X   (SLP_TIME_LABEL_X + 28)
#define SLP_TIME_ARROW_X (SLP_TIME_VAL_X + 28)
#define SLP_TIME_Y1      (SCHED_CHK_Y + SCHED_CHK_SZ + 2)  // 76
#define SLP_TIME_Y2      (SLP_TIME_Y1 + 11)                // 87

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

// Divider between sleep and brightness
#define DIV1_Y       (SLP_TIME_Y2 + 3)         // 90  (was 79)

// Section 2 — Brightness + Rotate + Page checkboxes (middle)
#define SEC2_X       8
#define SEC2_W       (SCREEN_W - 2 * SEC2_X)   // 304

#define BRI_LABEL_Y  (DIV1_Y + 4)               // 94  (was 83)
#define BRI_BTN_Y0   (BRI_LABEL_Y + 8)          // 102 (was 91)
#define BRI_BTN_H    18
#define BRI_BTN_W    ((SEC2_W - (BRI_LEVELS - 1) * 3) / BRI_LEVELS)  // 48

#define ROT_LABEL_Y  (BRI_BTN_Y0 + BRI_BTN_H + 3)   // 123 (was 112)
#define ROT_BTN_Y0   (ROT_LABEL_Y + 8)               // 131 (was 120)
#define ROT_BTN_H    18
#define ROT_BTN_COUNT 5
#define ROT_BTN_W    ((SEC2_W - (ROT_BTN_COUNT - 1) * 3) / ROT_BTN_COUNT)  // 58

#define PAGE_LABEL_Y (ROT_BTN_Y0 + ROT_BTN_H + 3)    // 152 (was 141)
#define PAGE_BTN_Y0  (PAGE_LABEL_Y + 8)               // 160 (was 149)
#define PAGE_BTN_H   18
#define PAGE_GAP     2
#define PAGE_COUNT   10
#define PAGE_BTN_W   ((SEC2_W - (PAGE_COUNT - 1) * PAGE_GAP) / PAGE_COUNT)  // 36

// Touch orientation — sits between PAGE and THEME sections
#define TOUCH_Y       (PAGE_BTN_Y0 + PAGE_BTN_H + 3)   // 181 (was 170)
#define TOUCH_H       12
#define RECAL_X       258
#define RECAL_W       52
#define RECAL_Y       TOUCH_Y
#define RECAL_H       TOUCH_H

// Divider between page/touch and theme
#define DIV2_Y       (TOUCH_Y + TOUCH_H + 2)            // 195 (was 183)

// Section 3 — Theme Color (bottom)
#define THEME_LABEL_Y (DIV2_Y + 2)          // 197 (was 185)
#define SWATCH_Y0     (THEME_LABEL_Y + 10)  // 207 (was 195)
#define SWATCH_H      18                     // shrunk from 20 to fit
#define SWATCH_W      ((SEC2_W - (THEME_COUNT - 1) * 2) / THEME_COUNT)  // 32
#define SWATCH_PAD    2

// ── Sleep constants ──────────────────────────────────────────────────────────
static const char *s_slpLabels[SLP_COUNT] = {"OFF","15m","30m","1h","2h","4h","8h"};
static const uint32_t s_slpMins[SLP_COUNT] = {0,15,30,60,120,240,480};

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
// Bit 0 = page 0 (NOW), bit 1 = page 1 (HOURLY), ..., bit 9 = page 9 (SCANNER)
static uint16_t s_pageMask  = 0x3FF;  // 10 pages (0-9)
static bool     s_pageLoaded = false;

static void pageMaskLoad() {
    if (s_pageLoaded) return;
    s_pageMask = (uint16_t)nvsGetInt("page_mask", 0x3FF);
    if (s_pageMask == 0) s_pageMask = 0x3FF;
    s_pageLoaded = true;
}

static void pageMaskSave() {
    nvsPutInt("page_mask", s_pageMask);
}

bool screenSettingsGetPageEnabled(int page) {
    pageMaskLoad();
    if (page < 0 || page >= PAGE_COUNT) return false;
    return (s_pageMask >> page) & 1;
}

int screenSettingsGetNextRotatePage(int current) {
    pageMaskLoad();
    if (s_pageMask == 0) return -1;
    for (int i = 1; i <= PAGE_COUNT; i++) {
        int p = (current + i) % PAGE_COUNT;
        if ((s_pageMask >> p) & 1) return p;
    }
    return -1;
}

// ── Sleep cache ───────────────────────────────────────────────────────────────
static int  s_slpTimer = -1;   // cached
static int  s_slpEnabled = -1;
static int  s_slpOnH = -1, s_slpOnM = -1, s_slpWakeH = -1, s_slpWakeM = -1;

static bool slpCacheLoad() {
    if (s_slpTimer < 0) {
        s_slpTimer   = nvsGetInt("slp_timer", 0);
        s_slpEnabled = nvsGetInt("slp_enable", 0);
        s_slpOnH     = nvsGetInt("slp_on_h", 22);
        s_slpOnM     = nvsGetInt("slp_on_m", 0);
        s_slpWakeH   = nvsGetInt("slp_wake_h", 7);
        s_slpWakeM   = nvsGetInt("slp_wake_m", 0);
    }
    return s_slpTimer >= 0;
}

static void slpCacheSave() {
    nvsPutInt("slp_timer",   s_slpTimer);
    nvsPutInt("slp_enable",  s_slpEnabled);
    nvsPutInt("slp_on_h",    s_slpOnH);
    nvsPutInt("slp_on_m",    s_slpOnM);
    nvsPutInt("slp_wake_h",  s_slpWakeH);
    nvsPutInt("slp_wake_m",  s_slpWakeM);
}

// ── Drawing ───────────────────────────────────────────────────────────────────
void screenSettingsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SETTINGS", timeStr, wifiOk);
    drawBottombar(tft, "", 10, 11);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    slpCacheLoad();

    // ── Section 1: Sleep Timer ──────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, SLP_LABEL_Y);
    tft.print("SLEEP TIMER");

    int activeSlp = s_slpTimer;
    for (int i = 0; i < SLP_COUNT; i++) {
        int bx = SEC2_X + i * (SLP_BTN_W + SLP_GAP);
        int by = SLP_BTN_Y0;
        tft.fillRect(bx, by, SLP_BTN_W, SLP_BTN_H, COL_INPUTBG);
        if (i == activeSlp) {
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

    // ── Section 1: Schedule ─────────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, SCHED_LABEL_Y);
    tft.print("SCHEDULE");

    // Night-mode checkbox
    {
        int cx = SCHED_CHK_X;
        int cy = SCHED_CHK_Y;
        tft.drawRect(cx, cy, SCHED_CHK_SZ, SCHED_CHK_SZ, s_slpEnabled ? g_themeColor : COL_DIM);
        if (s_slpEnabled)
            tft.fillRect(cx + 2, cy + 2, SCHED_CHK_SZ - 4, SCHED_CHK_SZ - 4, g_themeColor);

        tft.setTextColor(s_slpEnabled ? COL_WHITE : COL_DIM, COL_BG);
        tft.setCursor(cx + SCHED_CHK_SZ + 4, cy);
        tft.print("NIGHT MODE");

        // Show schedule status
        tft.setTextColor(COL_DIM, COL_BG);
        char schedBuf[24];
        snprintf(schedBuf, sizeof(schedBuf), "%s  %02d:%02d - %02d:%02d",
                 s_slpEnabled ? "ON" : "OFF",
                 s_slpOnH, s_slpOnM, s_slpWakeH, s_slpWakeM);
        int sw = tft.textWidth(schedBuf);
        tft.setCursor(SCREEN_W - SEC2_X - sw, cy);
        tft.print(schedBuf);
    }

    // ── Sleep time controls ───────────────────────────────────────────────────
    auto drawTimeControl = [&](int labelY, const char *label, int hour, int minute) {
        // Label
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(SLP_TIME_LABEL_X, labelY);
        tft.print(label);

        // Hour:Minute value
        char timeBuf[8];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hour, minute);
        tft.setTextColor(s_slpEnabled ? COL_WHITE : COL_DIM, COL_BG);
        tft.setCursor(SLP_TIME_VAL_X, labelY);
        tft.print(timeBuf);

        if (!s_slpEnabled) return;

        // Arrows
        const char *left  = "[<]";
        const char *right = "[>]";
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(SLP_TIME_ARROW_X, labelY);
        tft.print(left);
        tft.setCursor(SLP_TIME_ARROW_X + tft.textWidth(left) + 2, labelY);
        tft.print(right);
    };

    drawTimeControl(SLP_TIME_Y1, "SLEEP", s_slpOnH, s_slpOnM);
    drawTimeControl(SLP_TIME_Y2, "WAKE",  s_slpWakeH, s_slpWakeM);

    // Small "tap hour/minute" hint when schedule is on
    if (s_slpEnabled) {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(SLP_TIME_ARROW_X + 48, SLP_TIME_Y1);
        tft.print("tap H/M");
        tft.setCursor(SLP_TIME_ARROW_X + 48, SLP_TIME_Y2);
        tft.print("tap H/M");
    }

    // Divider 1
    tft.drawFastHLine(0, DIV1_Y, SCREEN_W, COL_DIM);

    // ── Right column: 2×2 grid ─────────────────────────────────────────────────
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
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, PAGE_LABEL_Y);
    tft.print("PAGE ROTATION");

    for (int i = 0; i < PAGE_COUNT; i++) {
        int bx  = SEC2_X + i * (PAGE_BTN_W + PAGE_GAP);
        int by  = PAGE_BTN_Y0;
        bool enabled = (s_pageMask >> i) & 1;

        tft.fillRect(bx, by, PAGE_BTN_W, PAGE_BTN_H, COL_INPUTBG);
        tft.drawRect(bx, by, PAGE_BTN_W, PAGE_BTN_H, enabled ? g_themeColor : COL_DIM);

        const int chkSize = 10;
        int chkX = bx + 3;
        int chkY = by + (PAGE_BTN_H - chkSize) / 2;
        tft.drawRect(chkX, chkY, chkSize, chkSize, enabled ? g_themeColor : COL_DIM);
        if (enabled)
            tft.fillRect(chkX + 2, chkY + 2, chkSize - 4, chkSize - 4, g_themeColor);

        char numBuf[3];
        snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        tft.setTextColor(enabled ? COL_WHITE : COL_DIM, COL_INPUTBG);
        tft.setCursor(chkX + chkSize + 3, by + (PAGE_BTN_H - 8) / 2);
        tft.print(numBuf);
    }

    // ── Section 2: Touch Orientation ──────────────────────────────────────────
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SEC2_X, TOUCH_Y);
    tft.print("TOUCH:");

    {
        int rot = touchGetRotation();
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(SEC2_X + tft.textWidth("TOUCH: "), TOUCH_Y);
        tft.print("ROT ");
        tft.print(rot);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(SEC2_X + tft.textWidth("TOUCH: ROT 0  "), TOUCH_Y);
        tft.print("(calibrated)");

        // Recalibrate button (right-aligned, same row)
        tft.fillRect(RECAL_X, RECAL_Y, RECAL_W, RECAL_H, COL_INPUTBG);
        tft.drawRect(RECAL_X, RECAL_Y, RECAL_W, RECAL_H, COL_AMBER);
        tft.setTextColor(COL_AMBER, COL_INPUTBG);
        const char *rl = "RECAL";
        int rlw = tft.textWidth(rl);
        tft.setCursor(RECAL_X + (RECAL_W - rlw) / 2, RECAL_Y + (RECAL_H - 8) / 2);
        tft.print(rl);
    }

    // Divider 2
    tft.drawFastHLine(0, DIV2_Y, SCREEN_W, COL_DIM);

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

    slpCacheLoad();

    // E-Ink toggle (top-left of right column)
    if (tx >= EINK_X && tx < EINK_X + BTN_SQ_W &&
        ty >= EINK_Y && ty < EINK_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        invertSet(!invertGet());
        return true;
    }

    // Power Off (top-right of right column) — two-tap confirmation
    if (tx >= PWR_X && tx < PWR_X + BTN_SQ_W &&
        ty >= PWR_Y && ty < PWR_Y + BTN_SQ_H) {
        if (s_pwrConfirm && millis() - s_pwrConfirmMs < 5000) {
            tft.fillScreen(COL_BG);
            tft.setTextFont(FONT_MD);
            tft.setTextColor(g_themeColor, COL_BG);
            tft.setCursor(80, 110);
            tft.print("Shutting down...");
            delay(500);
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
            esp_deep_sleep_start();
        } else {
            s_pwrConfirm = true;
            s_pwrConfirmMs = millis();
        }
        return true;
    }

    // Refresh Weather (bottom-left of right column)
    if (tx >= REFRESH_X && tx < REFRESH_X + BTN_SQ_W &&
        ty >= REFRESH_Y && ty < REFRESH_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        s_refreshRequested = true;
        return true;
    }

    // Update Location (bottom-right of right column)
    if (tx >= LOC_X && tx < LOC_X + BTN_SQ_W &&
        ty >= LOC_Y && ty < LOC_Y + BTN_SQ_H) {
        s_pwrConfirm = false;
        s_refreshRequested = true;
        return true;
    }

    // ── Sleep timer buttons ────────────────────────────────────────────
    for (int i = 0; i < SLP_COUNT; i++) {
        int bx = SEC2_X + i * (SLP_BTN_W + SLP_GAP);
        if (tx >= bx && tx < bx + SLP_BTN_W &&
            ty >= SLP_BTN_Y0 && ty < SLP_BTN_Y0 + SLP_BTN_H) {
            s_slpTimer = i;
            nvsPutInt("slp_timer", i);
            return true;
        }
    }

    // ── Night mode checkbox ────────────────────────────────────────────
    if (tx >= SCHED_CHK_X && tx < SCHED_CHK_X + SCHED_CHK_SZ &&
        ty >= SCHED_CHK_Y && ty < SCHED_CHK_Y + SCHED_CHK_SZ) {
        s_slpEnabled = !s_slpEnabled;
        nvsPutInt("slp_enable", s_slpEnabled);
        return true;
    }

    // ── Sleep/Wake time controls (only when enabled) ────────────────────
    if (s_slpEnabled) {
        auto handleTimeTap = [&](int labelY, int &hour, int &minute, const char *nvsKeyH, const char *nvsKeyM) {
            // Tap hour: increment by 1
            int hW = tft.textWidth("88:");  // width of "hh:"
            int valX = SLP_TIME_VAL_X;
            if (tx >= valX && tx < valX + hW && ty >= labelY && ty < labelY + 10) {
                hour = (hour + 1) % 24;
                return true;
            }
            // Tap minute: increment by 5
            int mX = valX + hW;
            if (tx >= mX && tx < mX + tft.textWidth("88") && ty >= labelY && ty < labelY + 10) {
                minute = (minute + 5) % 60;
                return true;
            }
            return false;
        };

        bool changed = false;
        changed |= handleTimeTap(SLP_TIME_Y1, s_slpOnH, s_slpOnM, "slp_on_h", "slp_on_m");
        changed |= handleTimeTap(SLP_TIME_Y2, s_slpWakeH, s_slpWakeM, "slp_wake_h", "slp_wake_m");
        if (changed) {
            slpCacheSave();
            return true;
        }
    }

    // ── Brightness ──────────────────────────────────────────────────────
    for (int i = 0; i < BRI_LEVELS; i++) {
        int bx = SEC2_X + i * (BRI_BTN_W + 3);
        if (tx >= bx && tx < bx + BRI_BTN_W &&
            ty >= BRI_BTN_Y0 && ty < BRI_BTN_Y0 + BRI_BTN_H) {
            brightnessSetLevel(i);
            return true;
        }
    }

    // ── Auto-rotate ────────────────────────────────────────────────────
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

    // ── Page rotation checkboxes ───────────────────────────────────────
    pageMaskLoad();
    for (int i = 0; i < PAGE_COUNT; i++) {
        int bx = SEC2_X + i * (PAGE_BTN_W + PAGE_GAP);
        if (tx >= bx && tx < bx + PAGE_BTN_W &&
            ty >= PAGE_BTN_Y0 && ty < PAGE_BTN_Y0 + PAGE_BTN_H) {
            s_pageMask ^= (1 << i);
            if (s_pageMask == 0) s_pageMask = (1 << i);
            pageMaskSave();
            return true;
        }
    }

    // ── Touch recalibrate button ────────────────────────────────────────
    if (tx >= RECAL_X && tx < RECAL_X + RECAL_W &&
        ty >= RECAL_Y && ty < RECAL_Y + RECAL_H) {
        nvsPutInt("cal_ver", -1);
        nvsPutInt("madctl", -1);
        nvsPutInt("touch_cal", 0);
        tft.fillScreen(COL_BG);
        tft.fillScreen(COL_BG);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(80, 110);
        tft.print("Recalibrating...");
        delay(500);
        ESP.restart();
        return true;
    }

    // ── Theme swatches ──────────────────────────────────────────────────
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

bool screenSettingsGetAutoRotate() {
    autoRotLoad();
    return s_autoRotSel > 0;
}

uint32_t screenSettingsGetAutoRotateMs() {
    autoRotLoad();
    return s_rotMs[s_autoRotSel];
}

int screenSettingsGetSleepTimerMins() {
    int idx = nvsGetInt("slp_timer", 0);
    if (idx < 0 || idx >= SLP_COUNT) return 0;
    return (int)s_slpMins[idx];
}
