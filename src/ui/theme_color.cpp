#include "theme_color.h"
#include "theme.h"
#include "../config/nvs_config.h"

const ThemeEntry g_themes[THEME_COUNT] = {
    { "CYAN",   0x07FFu },
    { "GREEN",  0x07E0u },
    { "RED",    0xF800u },
    { "ORANGE", 0xFC60u },
    { "YELLOW", 0xFFE0u },
    { "GRAY",   0x8410u },
    { "PURPLE", 0xA81Fu },
    { "PINK",   0xFB56u },
    { "WHITE",  0xFFFFu },
};

uint16_t g_themeColor = 0x07FFu;   // default cyan
static int g_themeIdx = 0;

void themeColorInit() {
    g_themeIdx = nvsGetInt("theme_idx", 0);
    if (g_themeIdx < 0 || g_themeIdx >= THEME_COUNT) g_themeIdx = 0;
    g_themeColor = g_themes[g_themeIdx].color;
}

void themeColorSet(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return;
    g_themeIdx  = idx;
    g_themeColor = g_themes[idx].color;
    nvsPutInt("theme_idx", idx);
}

int themeColorGetIdx() { return g_themeIdx; }
uint16_t themeColorGet() { return g_themeColor; }
