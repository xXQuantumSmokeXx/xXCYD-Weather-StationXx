#pragma once
#include <cstdint>

// ── Invert mode (white bg, black text — e-ink / flashlight) ──────────────
extern bool g_invert;

inline uint16_t colBg()    { return g_invert ? 0xFFFFu : 0x0000u; }
inline uint16_t colWhite() { return g_invert ? 0x0000u : 0xFFFFu; }
inline uint16_t colDim()   { return g_invert ? 0x7BEFu : 0x4208u; }
inline uint16_t colInputBg(){ return g_invert ? 0xDEFBu : 0x0841u; }

// ── Fixed accent colors (not inverted — semantic meaning preserved) ─────
#define COL_AMBER    0xFD40u
#define COL_RAIN     0x065Fu
#define COL_SNOW     0xAD75u
#define COL_STORM    0xFFE0u
#define COL_RED      0xF800u
#define COL_HUMIDITY 0x867Fu
#define COL_WIND     0x8410u

// ── Dynamic colors (respect g_invert) ────────────────────────────────────
#define COL_BG       colBg()
#define COL_WHITE    colWhite()
#define COL_DIM      colDim()
#define COL_INPUTBG  colInputBg()
#define COL_ALTROW   colBg()

// ── Fonts (TFT_eSPI built-in font IDs) ───────────────────────────────────
#define FONT_SM  1    //  8px — status labels
#define FONT_MD  2    // 16px — body text
#define FONT_LG  4    // 26px — section headers
#define FONT_NUM 7    // 48px — 7-segment style numbers (temp display)

// ── Theme color — runtime; invertSet() switches it to black/restored ─────
extern uint16_t g_themeColor;
