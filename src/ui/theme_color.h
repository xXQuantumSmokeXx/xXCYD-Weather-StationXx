#pragma once
#include <cstdint>

#define THEME_COUNT 9

struct ThemeEntry {
    const char *name;
    uint16_t    color;
};

extern const ThemeEntry g_themes[THEME_COUNT];

void     themeColorInit();
void     themeColorSet(int idx);
int      themeColorGetIdx();
uint16_t themeColorGet();

void     invertSet(bool on);
bool     invertGet();
