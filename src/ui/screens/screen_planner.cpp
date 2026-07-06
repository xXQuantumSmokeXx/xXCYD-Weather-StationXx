#include "screen_planner.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include "../../modules/weather.h"
#include <cstdio>
#include <cmath>
#include <ctime>

#define D2R (M_PI / 180.0)
#define ROW_H 19

static const char *rowLabels[9] = {
    "Astro dawn", "Naut dawn", "Civil dawn",
    "Sunrise",    "Solar noon","Sunset",
    "Civil dusk", "Naut dusk", "Astro dusk",
};

static const char *rowNotes[9] = {
    "True darkness ends",
    "Horizon visible",
    "Outdoor light begins",
    "Sun breaks the horizon",
    "Sun at highest point",
    "Golden hour begins",
    "Streetlights come on",
    "Stars visible overhead",
    "Deep sky observing",
};

static int dayOfYear() {
    time_t now = time(nullptr);
    if (now < 1000000) return 172;
    struct tm *lt = localtime(&now);
    return lt->tm_yday + 1;
}

static float solarDeclination(int doy) {
    return 23.44f * D2R * sinf(360.0f / 365.0f * (doy - 81) * D2R);
}

static float equationOfTime(int doy) {
    float B = 360.0f / 365.0f * (doy - 81) * D2R;
    return 9.87f * sinf(2.0f * B) - 7.53f * cosf(B) - 1.5f * sinf(B);
}

static float hourAngleForAltitude(float latRad, float decRad, float altRad) {
    float num = sinf(altRad) - sinf(latRad) * sinf(decRad);
    float den = cosf(latRad) * cosf(decRad);
    if (fabsf(den) < 0.00001f) return (num < 0) ? M_PI : 0.0f;
    float val = num / den;
    if (val > 1.0f) return 0.0f;
    if (val < -1.0f) return M_PI;
    return acosf(val);
}

static int timeFromNoonMins(float haRad, float eotMin) {
    float haDeg = haRad * 180.0f / M_PI;
    float mins = 720.0f + haDeg * 4.0f - eotMin;
    mins -= 4.0f * g_location.lon;            // longitude correction (lon is negative for W)
    mins += g_location.utcOffset / 60.0f;     // then shift to local wall-clock time
    while (mins < 0) mins += 1440;
    while (mins >= 1440) mins -= 1440;
    return (int)mins;
}

static void minsToHMA(char *out, size_t len, int mins) {
    int h = (mins / 60) % 24;
    int m = mins % 60;
    int am = (h >= 12) ? 'p' : 'a';
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    snprintf(out, len, "%d:%02d%c", h12, m, am);
}

static void drawRow(TFT_eSPI &tft, int y, const char *timeStr,
                    const char *label, const char *note, uint16_t color) {
    tft.setTextFont(FONT_MD);
    tft.setTextColor(color, COL_BG);
    tft.setCursor(6, y);
    tft.print(timeStr);

    tft.setTextFont(FONT_MD);
    tft.setCursor(58, y);
    tft.print(label);

    if (note && note[0]) {
        int nw = tft.textWidth(note);
        tft.setTextColor(color, COL_BG);
        tft.setCursor(SCREEN_W - 10 - nw, y);
        tft.print(note);
    }
}

void screenPlannerDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "ALMANAC", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 9, 11);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    int doy = dayOfYear();
    float latRad = g_location.lat * D2R;
    float decRad = solarDeclination(doy);
    float eotMin = equationOfTime(doy);

    float haCivil   = hourAngleForAltitude(latRad, decRad, -6.0f  * D2R);
    float haNaut    = hourAngleForAltitude(latRad, decRad, -12.0f * D2R);
    float haAstro   = hourAngleForAltitude(latRad, decRad, -18.0f * D2R);
    float haSunrise = hourAngleForAltitude(latRad, decRad, -0.833f * D2R);

    int sunriseM = timeFromNoonMins(-haSunrise, eotMin);
    int sunsetM  = timeFromNoonMins(+haSunrise, eotMin);
    int civStart = timeFromNoonMins(-haCivil, eotMin);
    int civEnd   = timeFromNoonMins(+haCivil, eotMin);
    int nautStart = timeFromNoonMins(-haNaut, eotMin);
    int nautEnd  = timeFromNoonMins(+haNaut, eotMin);
    int astStart = timeFromNoonMins(-haAstro, eotMin);
    int astEnd   = timeFromNoonMins(+haAstro, eotMin);
    int noonM    = (sunriseM + sunsetM) / 2;

    char sunR[8], noon[8], sunS[8];
    char astD[8], nauD[8], civD[8], civS[8], nauS[8], astS[8];

    if (g_daily[0].sunset[0]) {
        snprintf(sunR, sizeof(sunR), "%s", g_daily[0].sunrise);
        snprintf(sunS, sizeof(sunS), "%s", g_daily[0].sunset);
    } else {
        minsToHMA(sunR, sizeof(sunR), sunriseM);
        minsToHMA(sunS, sizeof(sunS), sunsetM);
    }
    minsToHMA(astD, sizeof(astD), astStart);
    minsToHMA(nauD, sizeof(nauD), nautStart);
    minsToHMA(civD, sizeof(civD), civStart);
    minsToHMA(noon, sizeof(noon), noonM);
    minsToHMA(civS, sizeof(civS), civEnd);
    minsToHMA(nauS, sizeof(nauS), nautEnd);
    minsToHMA(astS, sizeof(astS), astEnd);

    const char *times[9] = {astD, nauD, civD, sunR, noon, sunS, civS, nauS, astS};

    char buf[48];
    int y = CONTENT_Y + 2;

    int dayLen = sunsetM - sunriseM;
    if (dayLen > 0) {
        int h = dayLen / 60, m = dayLen % 60;
        snprintf(buf, sizeof(buf), "%dh %02dm of daylight", h, m);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        int tw = tft.textWidth(buf);
        tft.setCursor((SCREEN_W - tw) / 2, y);
        tft.print(buf);
        y += 18;
    }
    tft.drawFastHLine(40, y, SCREEN_W - 80, g_themeColor);
    y += 5;

    for (int i = 0; i < 9; i++) {
        drawRow(tft, y, times[i], rowLabels[i], rowNotes[i], g_themeColor);
        y += ROW_H;
    }
}

void screenPlannerTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H) { /* trigger redraw via caller */ }
}
