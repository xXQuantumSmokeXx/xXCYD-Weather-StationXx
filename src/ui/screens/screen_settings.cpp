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
#include <WiFi.h>
#include <esp_sleep.h>
#include <cstdio>
#include <cstring>

// ── Layout constants ──────────────────────────────────────────────────────────
// Section 1 — Info (top)
#define INFO_X       8
#define INFO_Y0      (CONTENT_Y + 4)    // 27
#define INFO_LINE_H  12

// E-Ink + Power buttons — right column of info section
#define EINK_X       195
#define EINK_Y       INFO_Y0                    // 27
#define EINK_W       (SCREEN_W - EINK_X - 8)   // ~117
#define EINK_H       23
#define PWR_Y        (EINK_Y + EINK_H + 2)     // 52
#define PWR_H        23

// Divider between info and controls
#define DIV1_Y       (INFO_Y0 + 4 * INFO_LINE_H + 3)   // 79

// Section 2 — Brightness + Rotate + Action buttons (middle)
#define SEC2_X       8
#define SEC2_W       (SCREEN_W - 2 * SEC2_X)   // 304

#define BRI_LABEL_Y  (DIV1_Y + 4)               // 83
#define BRI_BTN_Y0   (BRI_LABEL_Y + 10)         // 93
#define BRI_BTN_H    18
#define BRI_BTN_W    ((SEC2_W - (BRI_LEVELS - 1) * 3) / BRI_LEVELS)  // 48

#define ROT_LABEL_Y  (BRI_BTN_Y0 + BRI_BTN_H + 4)   // 115
#define ROT_BTN_Y0   (ROT_LABEL_Y + 10)              // 125
#define ROT_BTN_H    18
#define ROT_BTN_COUNT 5
#define ROT_BTN_W    ((SEC2_W - (ROT_BTN_COUNT - 1) * 3) / ROT_BTN_COUNT)  // 58

#define BTN_Y        (ROT_BTN_Y0 + ROT_BTN_H + 5)   // 148
#define BTN_H        20
#define BTN_W        ((SEC2_W - 8) / 2)              // 148

// Divider between controls and theme
#define DIV2_Y       (BTN_Y + BTN_H + 4)   // 172

// Section 3 — Theme Color (bottom)
#define THEME_LABEL_Y (DIV2_Y + 4)          // 176
#define SWATCH_Y0     (THEME_LABEL_Y + 14)  // 190
#define SWATCH_H      20
#define SWATCH_W      ((SEC2_W - (THEME_COUNT - 1) * 2) / THEME_COUNT)  // 32
#define SWATCH_PAD    2

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

void screenSettingsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SETTINGS", timeStr, wifiOk);
    drawBottombar(tft, "", 7, 8);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    // ── Section 1: Info ───────────────────────────────────────────────────────
    tft.setTextFont(FONT_SM);

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(INFO_X, INFO_Y0);
    tft.print("WiFi: ");
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.print(WiFi.SSID().c_str());

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(INFO_X, INFO_Y0 + INFO_LINE_H);
    tft.print("IP:   ");
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.print(WiFi.localIP().toString().c_str());

    if (g_location.valid) {
        char locBuf[32];
        snprintf(locBuf, sizeof(locBuf), "%.3f, %.3f", g_location.lat, g_location.lon);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(INFO_X, INFO_Y0 + INFO_LINE_H * 2);
        tft.print("Loc:  ");
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.print(locBuf);
    }

    // Battery bar
    {
        int batY = INFO_Y0 + INFO_LINE_H * 3;
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(INFO_X, batY);
        tft.print("BAT:  ");
        int lblW = tft.textWidth("BAT:  ");
        int bpct = batteryPct();
        if (bpct >= 0) {
            const int barX = INFO_X + lblW;
            const int barW = 80;
            const int barH = 7;
            const int barY = batY + (8 - barH) / 2;
            uint16_t barCol = (bpct < 20) ? COL_AMBER : g_themeColor;
            tft.drawRect(barX, barY, barW, barH, COL_DIM);
            int fill = (barW - 2) * bpct / 100;
            if (fill > 0) tft.fillRect(barX + 1, barY + 1, fill, barH - 2, barCol);
            tft.fillRect(barX + barW, barY + 2, 3, barH - 4, COL_DIM);
            char batBuf[8];
            snprintf(batBuf, sizeof(batBuf), " %d%%", bpct);
            tft.setTextColor(COL_WHITE, COL_BG);
            tft.setCursor(barX + barW + 5, batY);
            tft.print(batBuf);
        } else {
            tft.setTextColor(COL_DIM, COL_BG);
            tft.setCursor(INFO_X + tft.textWidth("BAT:  "), batY);
            tft.print("N/A");
        }
    }

    // ── E-Ink / Flashlight toggle — right column, top ──────────────────────────
    {
        tft.fillRect(EINK_X, EINK_Y, EINK_W, EINK_H, COL_INPUTBG);
        if (invertGet()) {
            tft.drawRect(EINK_X - 1, EINK_Y - 1, EINK_W + 2, EINK_H + 2, COL_WHITE);
            tft.drawRect(EINK_X - 2, EINK_Y - 2, EINK_W + 4, EINK_H + 4, COL_WHITE);
            tft.setTextColor(COL_BG, COL_INPUTBG);
        } else {
            tft.drawRect(EINK_X, EINK_Y, EINK_W, EINK_H, g_themeColor);
            tft.setTextColor(g_themeColor, COL_INPUTBG);
        }
        tft.setTextFont(FONT_MD);
        char einkLabel[12];
        snprintf(einkLabel, sizeof(einkLabel), "E-INK %s", invertGet() ? "ON" : "OFF");
        int elw = tft.textWidth(einkLabel);
        tft.setCursor(EINK_X + (EINK_W - elw) / 2, EINK_Y + (EINK_H - 16) / 2);
        tft.print(einkLabel);
    }

    // ── Power Off — right column, bottom ───────────────────────────────────────
    {
        tft.fillRect(EINK_X, PWR_Y, EINK_W, PWR_H, COL_INPUTBG);
        tft.drawRect(EINK_X, PWR_Y, EINK_W, PWR_H, COL_RED);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_RED, COL_INPUTBG);
        const char *pwrLabel = s_pwrConfirm ? "SURE?" : "PWR OFF";
        int plw = tft.textWidth(pwrLabel);
        tft.setCursor(EINK_X + (EINK_W - plw) / 2, PWR_Y + (PWR_H - 16) / 2);
        tft.print(pwrLabel);
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

    // ── Section 2: Action buttons ─────────────────────────────────────────────
    tft.fillRect(SEC2_X, BTN_Y, BTN_W, BTN_H, COL_INPUTBG);
    tft.drawRect(SEC2_X, BTN_Y, BTN_W, BTN_H, g_themeColor);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_INPUTBG);
    tft.setCursor(SEC2_X + 6, BTN_Y + (BTN_H - 8) / 2);
    tft.print("REFRESH WEATHER");

    int btn2x = SEC2_X + BTN_W + 8;
    tft.fillRect(btn2x, BTN_Y, BTN_W, BTN_H, COL_INPUTBG);
    tft.drawRect(btn2x, BTN_Y, BTN_W, BTN_H, COL_AMBER);
    tft.setTextColor(COL_AMBER, COL_INPUTBG);
    tft.setCursor(btn2x + 6, BTN_Y + (BTN_H - 8) / 2);
    tft.print("UPDATE LOCATION");

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

bool screenSettingsTap(TFT_eSPI &tft, int16_t tx, int16_t ty) {
    // Timeout stale power-off confirmation
    if (s_pwrConfirm && millis() - s_pwrConfirmMs > 5000) {
        s_pwrConfirm = false;
    }

    // E-Ink toggle button
    if (tx >= EINK_X && tx < EINK_X + EINK_W &&
        ty >= EINK_Y && ty < EINK_Y + EINK_H) {
        s_pwrConfirm = false;
        invertSet(!invertGet());
        return true;
    }

    // Power Off button — two-tap confirmation
    if (tx >= EINK_X && tx < EINK_X + EINK_W &&
        ty >= PWR_Y  && ty < PWR_Y + PWR_H) {
        if (s_pwrConfirm && millis() - s_pwrConfirmMs < 5000) {
            // Second tap within 5s — deep sleep
            tft.fillScreen(COL_BG);
            tft.setTextFont(FONT_MD);
            tft.setTextColor(g_themeColor, COL_BG);
            tft.setCursor(80, 110);
            tft.print("Shutting down...");
            delay(500);
            esp_deep_sleep_start();
        } else {
            s_pwrConfirm = true;
            s_pwrConfirmMs = millis();
        }
        return true;
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

    // Refresh button
    if (tx >= SEC2_X && tx < SEC2_X + BTN_W &&
        ty >= BTN_Y   && ty < BTN_Y + BTN_H)
        s_refreshRequested = true;

    // Location button
    int btn2x = SEC2_X + BTN_W + 8;
    if (tx >= btn2x && tx < btn2x + BTN_W &&
        ty >= BTN_Y  && ty < BTN_Y + BTN_H)
        s_refreshRequested = true;

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

bool screenSettingsGetAutoRotate() {
    autoRotLoad();
    return s_autoRotSel > 0;
}

uint32_t screenSettingsGetAutoRotateMs() {
    autoRotLoad();
    return s_rotMs[s_autoRotSel];
}
