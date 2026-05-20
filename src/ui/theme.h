#pragma once
#include <cstdint>

// ── Black background — all screens ───────────────────────────────────────
#define COL_BG       0x0000u   // pure black

// ── Fixed accent colors ───────────────────────────────────────────────────
#define COL_WHITE    0xFFFFu
#define COL_DIM      0x4208u   // dim grey for labels
#define COL_AMBER    0xFD40u   // warnings / frost
#define COL_RAIN     0x065Fu   // rain drops / precip
#define COL_SNOW     0xAD75u   // snow icon
#define COL_STORM    0xFFE0u   // lightning yellow
#define COL_RED      0xF800u   // high temps / storm alert
#define COL_INPUTBG  0x0841u   // very dark input field bg
#define COL_HUMIDITY 0x867Fu   // light blue — humidity
#define COL_WIND     0x8410u   // gray — wind speed
#define COL_ALTROW   0x0000u   // true black (matches COL_BG)

// ── Fonts (TFT_eSPI / LovyanGFX built-in font IDs) ───────────────────────
#define FONT_SM  1    //  8px — status labels
#define FONT_MD  2    // 16px — body text
#define FONT_LG  4    // 26px — section headers
#define FONT_NUM 7    // 48px — 7-segment style numbers (temp display)

// ── Theme color — runtime, set by themeColorGet() ────────────────────────
extern uint16_t g_themeColor;
