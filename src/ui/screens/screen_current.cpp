#include "screen_current.h"
#include "../theme.h"
#include "../widgets.h"
#include "../icons.h"
#include "../../config/config.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <cstdio>
#include <math.h>
#include <time.h>

// Lunar phase: 0.0 = new, 0.5 = full, 1.0 = new
static float getMoonPhase() {
    time_t now = time(nullptr);
    const time_t knownNewMoon = 947182440;   // Jan 6 2000 18:14 UTC
    const double lunarCycle   = 29.53058867 * 86400.0;
    double phase = fmod(difftime(now, knownNewMoon), lunarCycle) / lunarCycle;
    if (phase < 0.0) phase += 1.0;
    return (float)phase;
}

static const char* moonPhaseName(float phase) {
    if (phase < 0.04f || phase > 0.96f) return "NEW MOON";
    if (phase < 0.23f) return "WAX CRSC";
    if (phase < 0.27f) return "1ST QTR";
    if (phase < 0.48f) return "WAX GIBB";
    if (phase < 0.52f) return "FULL MOON";
    if (phase < 0.73f) return "WAN GIBB";
    if (phase < 0.77f) return "3RD QTR";
    return "WAN CRSC";
}

static void drawMoonPhase(TFT_eSPI &tft, int cx, int cy, int r, float phase) {
    tft.fillCircle(cx, cy, r, COL_BG);
    for (int dy = -(r - 1); dy <= r - 1; dy++) {
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        if (hw == 0) continue;
        int x1, x2;
        if (phase <= 0.5f) {
            int termX = (int)(hw * (1.0f - 4.0f * phase));
            x1 = cx + termX;  x2 = cx + hw;
        } else {
            int termX = (int)(hw * (3.0f - 4.0f * phase));
            x1 = cx - hw;     x2 = cx + termX;
        }
        if (x2 > x1)
            tft.drawFastHLine(x1, cy + dy, x2 - x1, COL_WHITE);
    }
    tft.drawCircle(cx, cy, r, COL_DIM);
}

void screenCurrentDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "NOW", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 0, 4);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (!g_current.valid) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(60, 110);
        tft.print("No weather data");
        return;
    }

    // ── Layout ───────────────────────────────────────────────────────────
    const int ICX    = 56;
    const int ICY    = CONTENT_Y + 28;   // moved up to make room for font-7 HI/LO
    const int ICR    = 24;               // reduced from 34 to fit HI/LO in font 7
    const int DX     = 118;
    const int DW     = SCREEN_W - DX - 4;
    const int LEFT_W = DX - 8;           // 110px
    const int MCX    = DX + 157;         // moon center x = 275
    const int MCY    = CONTENT_Y + 38;   // moon center y = 61
    const int MOON_R = 22;

    tft.drawFastVLine(DX - 8, CONTENT_Y + 4, CONTENT_H - 8, g_themeColor);

    // ── LEFT: weather icon + condition ───────────────────────────────────
    drawWmoIcon(tft, g_current.weather_code, ICX, ICY, ICR, g_themeColor);
    const char *cond = wmoDescription(g_current.weather_code);
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    int cw = tft.textWidth(cond);
    tft.setCursor((LEFT_W - cw) / 2, ICY + ICR + 5);
    tft.print(cond);

    // ── LEFT: HI / LO in font 7 (7-seg, same style as main temp) ────────
    // Positions tight-fit for ICR=24: condY ends at ~96, loVal ends at ~215
    const int condY  = ICY + ICR + 5;
    const int hiLblY = condY + 16 + 4;   // 4px gap after condition text
    const int hiValY = hiLblY + 8;
    const int loLblY = hiValY + 48 + 3;
    const int loValY = loLblY + 8;

    char numBuf[8];

    // HI label
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    int hlw = tft.textWidth("HI");
    tft.setCursor((LEFT_W - hlw) / 2, hiLblY);
    tft.print("HI");

    // HI value — font 7, centered in left column
    tft.setTextFont(7);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(numBuf, sizeof(numBuf), "%d", (int)roundf(g_current.today_max));
    int hiW = tft.textWidth(numBuf);
    tft.setCursor((LEFT_W - hiW) / 2, hiValY);
    tft.print(numBuf);

    // LO label
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    int llw = tft.textWidth("LO");
    tft.setCursor((LEFT_W - llw) / 2, loLblY);
    tft.print("LO");

    // LO value — font 7, centered
    tft.setTextFont(7);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(numBuf, sizeof(numBuf), "%d", (int)roundf(g_current.today_min));
    int loW = tft.textWidth(numBuf);
    tft.setCursor((LEFT_W - loW) / 2, loValY);
    tft.print(numBuf);

    // ── RIGHT: large temperature ─────────────────────────────────────────
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", (int)roundf(g_current.temp));
    tft.setTextFont(7);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(DX, CONTENT_Y + 12);
    tft.print(buf);
    int tw = tft.textWidth(buf);
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(DX + tw + 2, CONTENT_Y + 16);
    tft.print("\xB0""F");

    // Feels like — value in white, then themed F
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(DX, CONTENT_Y + 64);
    tft.print("feels like ");
    int lw = tft.textWidth("feels like ");
    snprintf(buf, sizeof(buf), "%d\xB0", (int)roundf(g_current.feels_like));
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(DX + lw, CONTENT_Y + 64);
    tft.print(buf);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.print("F");

    // Separator
    tft.drawFastHLine(DX, CONTENT_Y + 84, DW, g_themeColor);

    // ── Moon — phase name, disk, illumination % (all above separator) ────
    float moonPhase = getMoonPhase();
    float illum     = 0.5f * (1.0f - cosf(moonPhase * 2.0f * 3.14159265f));
    int   illumPct  = (int)(illum * 100.0f + 0.5f);
    float daysToFull = (moonPhase <= 0.5f)
        ? (0.5f - moonPhase) * 29.53f
        : (1.5f - moonPhase) * 29.53f;

    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_WHITE, COL_BG);
    const char *pname = moonPhaseName(moonPhase);
    int pnw = tft.textWidth(pname);
    tft.setCursor(MCX - pnw / 2, CONTENT_Y + 6);
    tft.print(pname);

    drawMoonPhase(tft, MCX, MCY, MOON_R, moonPhase);

    // Illumination % — FONT_MD for readability
    snprintf(buf, sizeof(buf), "%d%%", illumPct);
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    int ilw = tft.textWidth(buf);
    tft.setCursor(MCX - ilw / 2, MCY + MOON_R + 5);
    tft.print(buf);

    // ── Stats: 2-col × 4-row, stacked label(FONT_SM) + value(FONT_MD) ────
    // SLH=26 fills separator→content-bottom: 4×26=104, y=111→215
    // Left:  UV | HUM | WIND | BARO
    // Right: TO FULL | VIS | SUNRISE | SUNSET
    const int SY0 = CONTENT_Y + 88;   // y=111
    const int SLH = 26;               // label(8) + value(16) + gap(2)
    const int SCL = DX;               // left col  = 118
    const int SCR = DX + 99;          // right col = 217

    const char *lbl_L[4] = { "UV",       "HUM",     "WIND",    "BARO"    };
    const char *lbl_R[4] = { "TO FULL",  "VIS",     "SUNRISE", "SUNSET"  };
    char        val_L[4][24];
    char        val_R[4][24];

    snprintf(val_L[0], 24, "%.0f",     g_current.uv_index);
    snprintf(val_L[1], 24, "%d%%",     g_current.humidity);
    snprintf(val_L[2], 24, "%d mph %s",(int)roundf(g_current.wind_speed), windCardinal(g_current.wind_dir));
    snprintf(val_L[3], 24, "%d hPa",   (int)roundf(g_current.pressure));

    snprintf(val_R[0], 24, "%.0f d",   daysToFull);
    snprintf(val_R[1], 24, "%.1f mi",  g_current.visibility);
    snprintf(val_R[2], 24, "%s",       g_daily[0].sunrise[0] ? g_daily[0].sunrise : "--:--");
    snprintf(val_R[3], 24, "%s",       g_daily[0].sunset[0]  ? g_daily[0].sunset  : "--:--");

    for (int i = 0; i < 4; i++) {
        int ly = SY0 + i * SLH;
        int vy = ly + 8;

        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(SCL, ly); tft.print(lbl_L[i]);
        tft.setCursor(SCR, ly); tft.print(lbl_R[i]);

        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        tft.setCursor(SCL, vy); tft.print(val_L[i]);
        tft.setCursor(SCR, vy); tft.print(val_R[i]);
    }
}
