#include "theme_color.h"
#include "theme.h"
#include "../config/nvs_config.h"
#include "../modules/brightness.h"
#include <Arduino.h>

const ThemeEntry g_themes[THEME_COUNT] = {
    { "CYAN",   0x07FFu },   // max green+blue — already neon
    { "GREEN",  0x07E0u },   // max green — already neon
    { "RED",    0xF810u },   // neon red: pure R + hint of blue
    { "ORANGE", 0xFE80u },   // neon orange: max R + strong G
    { "YELLOW", 0xFFE0u },   // max red+green — already neon
    { "GRAY",   0xAD55u },   // neutral mid-gray (was green-tinted)
    { "PURPLE", 0xF01Fu },   // neon purple: max B + strong R
    { "PINK",   0xFE5Au },   // hot pink: max R + strong G + blue
    { "WHITE",  0xFFFFu },   // full white
};

uint16_t g_themeColor  = 0x07FFu;   // current active color (black when inverted)
bool     g_invert      = false;
static uint16_t s_themeStored = 0x07FFu;  // user's chosen color, preserved across invert
static int      g_themeIdx    = 0;

void themeColorInit() {
    g_themeIdx = nvsGetInt("theme_idx", 0);
    if (g_themeIdx < 0 || g_themeIdx >= THEME_COUNT) g_themeIdx = 0;
    s_themeStored = g_themes[g_themeIdx].color;
    g_invert = nvsGetInt("invert", 0) != 0;
    g_themeColor = g_invert ? 0x0000u : s_themeStored;
}

void themeColorSet(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_themeIdx    = idx;
    s_themeStored = g_themes[idx].color;
    if (!g_invert) g_themeColor = s_themeStored;
    nvsPutInt("theme_idx", idx);
}

int      themeColorGetIdx() { return g_themeIdx; }
uint16_t themeColorGet()    { return s_themeStored; }  // always returns stored (non-inverted)

void invertSet(bool on) {
    if (on == g_invert) return;
    g_invert = on;
    g_themeColor = on ? 0x0000u : s_themeStored;
    // Flashlight: max backlight when inverted, restore previous when off
    if (on) ledcWrite(0, 255);
    else    brightnessRestore();
    nvsPutInt("invert", on ? 1 : 0);
}

bool invertGet() { return g_invert; }
