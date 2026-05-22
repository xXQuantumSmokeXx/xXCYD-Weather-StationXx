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

extern bool g_spriteCapture;

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
    drawBottombar(tft, dateStr, 0, 7);
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
    const int hiValY = condY + 16 + 4;   // 4px gap after condition text
    const int loValY = hiValY + 59;
    const int tempLblDy = 20;

    char numBuf[8];

    // HI value with label left-centered
    tft.setTextFont(g_spriteCapture ? FONT_LG : 7);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(numBuf, sizeof(numBuf), "%d", (int)roundf(g_current.today_max));
    int hiW = tft.textWidth(numBuf);
    int hiX = (LEFT_W - hiW) / 2;
    tft.setCursor(hiX, hiValY);
    tft.print(numBuf);

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    int hlw = tft.textWidth("HI");
    int hiLblX = hiX - hlw - 4;
    if (hiLblX < 0) hiLblX = 0;
    tft.setCursor(hiLblX, hiValY + tempLblDy);
    tft.print("HI");

    // LO value with label left-centered
    tft.setTextFont(g_spriteCapture ? FONT_LG : 7);
    tft.setTextColor(COL_WHITE, COL_BG);
    snprintf(numBuf, sizeof(numBuf), "%d", (int)roundf(g_current.today_min));
    int loW = tft.textWidth(numBuf);
    int loX = (LEFT_W - loW) / 2;
    tft.setCursor(loX, loValY);
    tft.print(numBuf);

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    int llw = tft.textWidth("LO");
    int loLblX = loX - llw - 4;
    if (loLblX < 0) loLblX = 0;
    tft.setCursor(loLblX, loValY + tempLblDy);
    tft.print("LO");

    // RIGHT: large temperature
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", (int)roundf(g_current.temp));
    tft.setTextFont(g_spriteCapture ? FONT_LG : 7);
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

    // ── Stats: 2-col × 4-row, label(FONT_SM) + value(FONT_MD) ────
    // SLH=26 fills separator→content-bottom: 4×26=104, y=111→215
    // Left:  UV INDEX | HUMIDITY | WINDSPEED | BAROMETER
    // Right: TO FULL MOON | VISIBILITY | SUNRISE | SUNSET
    //
    // "centered" items: value centered under label
    // "left" items:     label+value left-aligned at column start
    const int SY0 = CONTENT_Y + 88;
    const int SLH = 26;
    const int SCL = DX;
    const int SCR = DX + 94;

    struct StatRow { const char *label; const char *val; bool centered; };
    char v0[24], v1[24], v2[24], v3[24], v4[24], v5[24], v6[24], v7[24];
    snprintf(v0, 24, "%.0f",     g_current.uv_index);
    snprintf(v1, 24, "%d%%",     g_current.humidity);
    snprintf(v2, 24, "%d mph %s",(int)roundf(g_current.wind_speed), windCardinal(g_current.wind_dir));
    snprintf(v3, 24, "%d hPa",   (int)roundf(g_current.pressure));
    snprintf(v4, 24, "%.0f d",   daysToFull);
    snprintf(v5, 24, "%.1f mi",  g_current.visibility);
    snprintf(v6, 24, "%s",       g_daily[0].sunrise[0] ? g_daily[0].sunrise : "--:--");
    snprintf(v7, 24, "%s",       g_daily[0].sunset[0]  ? g_daily[0].sunset  : "--:--");

    StatRow left[4] = {
        { "UV INDEX",    v0, false },
        { "HUMIDITY",    v1, false },
        { "WINDSPEED",   v2, false },
        { "BAROMETER",   v3, false },
    };
    StatRow right[4] = {
        { "FULL MOON",    v4, false },
        { "VISIBILITY",   v5, false },
        { "SUNRISE",      v6, false },
        { "SUNSET",       v7, false },
    };

    for (int i = 0; i < 4; i++) {
        int ly = SY0 + i * SLH;
        int vy = ly + 8;

        // Left column
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(SCL, ly); tft.print(left[i].label);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        if (left[i].centered) {
            int lw = tft.textWidth(left[i].label);
            int vw = tft.textWidth(left[i].val);
            tft.setCursor(SCL + (lw - vw) / 2, vy);
        } else {
            tft.setCursor(SCL, vy);
        }
        tft.print(left[i].val);

        // Right column
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(SCR, ly); tft.print(right[i].label);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        if (right[i].centered) {
            int lw = tft.textWidth(right[i].label);
            int vw = tft.textWidth(right[i].val);
            tft.setCursor(SCR + (lw - vw) / 2, vy);
        } else {
            tft.setCursor(SCR, vy);
        }
        tft.print(right[i].val);
    }
}
