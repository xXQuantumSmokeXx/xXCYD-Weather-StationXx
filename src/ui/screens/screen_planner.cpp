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

// Hour angle in radians for a given solar altitude (altRad: -6=civil, -12=nautical, -18=astro)
static float hourAngleForAltitude(float latRad, float decRad, float altRad) {
    float num = sinf(altRad) - sinf(latRad) * sinf(decRad);
    float den = cosf(latRad) * cosf(decRad);
    if (fabsf(den) < 0.00001f) return (num < 0) ? M_PI : 0.0f;
    float val = num / den;
    if (val > 1.0f) return 0.0f;
    if (val < -1.0f) return M_PI;
    return acosf(val);
}

// Time from midnight (minutes) for a given hour angle, EoT, and UTC offset
static int timeFromNoonMins(float haRad, float eotMin) {
    float haDeg = haRad * 180.0f / M_PI;
    float mins = 720.0f + haDeg * 4.0f - eotMin;
    mins += g_location.utcOffset / 60.0f;
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

static float moonPhase() {
    time_t now = time(nullptr);
    if (now < 1000000) return 0.5f;
    const time_t newMoon = 947182440;
    const double cycle = 29.53058867 * 86400.0;
    double phase = fmod(difftime(now, newMoon), cycle) / cycle;
    if (phase < 0.0) phase += 1.0;
    return (float)phase;
}

static const char *moonPhaseName(float phase) {
    if (phase < 0.03f || phase > 0.97f) return "NEW MOON";
    if (phase < 0.22f) return "WAX CRESC";
    if (phase < 0.28f) return "FIRST QTR";
    if (phase < 0.47f) return "WAX GIBB";
    if (phase < 0.53f) return "FULL MOON";
    if (phase < 0.72f) return "WAN GIBB";
    if (phase < 0.78f) return "LAST QTR";
    return "WAN CRESC";
}

static void drawSection(TFT_eSPI &tft, int &y, const char *title) {
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, y);
    tft.print(title);
    tft.drawFastHLine(8, y + 16, SCREEN_W - 16, COL_DIM);
    y += 20;
}

static void drawTimeRow(TFT_eSPI &tft, int &y, const char *label, const char *time1, const char *time2 = nullptr) {
    tft.setTextFont(FONT_SM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(12, y + 3);
    tft.print(label);
    tft.setTextFont(FONT_MD);
    tft.setTextColor(g_themeColor, COL_BG);
    if (time2) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s - %s", time1, time2);
        int w = tft.textWidth(buf);
        tft.setCursor(SCREEN_W - 8 - w, y);
        tft.print(buf);
    } else {
        int w = tft.textWidth(time1);
        tft.setCursor(SCREEN_W - 8 - w, y);
        tft.print(time1);
    }
    y += 22;
}

void screenPlannerDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "PLANNER", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 8, 11);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    int doy = dayOfYear();
    float latRad = g_location.lat * D2R;
    float decRad = solarDeclination(doy);
    float eotMin = equationOfTime(doy);

    // Twilight hour angles
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

    char buf[16], buf2[16];

    // Sunrise/Sunset section
    int y = CONTENT_Y + 6;
    drawSection(tft, y, "SUN");

    if (g_daily[0].sunset[0]) {
        // Use Open-Meteo if available (more accurate with terrain refraction)
        drawTimeRow(tft, y, "Sunrise", g_daily[0].sunrise);
        drawTimeRow(tft, y, "Sunset",  g_daily[0].sunset);
    } else {
        minsToHMA(buf, sizeof(buf), sunriseM);
        minsToHMA(buf2, sizeof(buf2), sunsetM);
        drawTimeRow(tft, y, "Sunrise", buf);
        drawTimeRow(tft, y, "Sunset",  buf2);
    }
    minsToHMA(buf, sizeof(buf), noonM);
    drawTimeRow(tft, y, "Solar Noon", buf);
    y += 2;

    // Twilight section
    drawSection(tft, y, "TWILIGHT");
    minsToHMA(buf, sizeof(buf), civStart);
    minsToHMA(buf2, sizeof(buf2), civEnd);
    drawTimeRow(tft, y, "Civil", buf, buf2);
    minsToHMA(buf, sizeof(buf), nautStart);
    minsToHMA(buf2, sizeof(buf2), nautEnd);
    drawTimeRow(tft, y, "Nautical", buf, buf2);
    minsToHMA(buf, sizeof(buf), astStart);
    minsToHMA(buf2, sizeof(buf2), astEnd);
    drawTimeRow(tft, y, "Astro", buf, buf2);
    y += 2;

    // Moon section
    drawSection(tft, y, "MOON");
    float phase = moonPhase();
    float illum = (1.0f - cosf(phase * 2.0f * M_PI)) / 2.0f;
    const char *phaseStr = moonPhaseName(phase);

    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(12, y);
    tft.print(phaseStr);
    snprintf(buf, sizeof(buf), "%.0f%% illum", illum * 100.0f);
    tft.setTextFont(FONT_SM);
    int w = tft.textWidth(buf);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(SCREEN_W - 8 - w, y + 3);
    tft.print(buf);
    y += 24;

    // Day length
    int dayLen = sunsetM - sunriseM;
    if (dayLen > 0) {
        y += 4;
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        int h = dayLen / 60, m = dayLen % 60;
        snprintf(buf, sizeof(buf), "%dh %02dm day", h, m);
        int tw = tft.textWidth(buf);
        tft.setCursor((SCREEN_W - tw) / 2, y);
        tft.print(buf);
    }
}

void screenPlannerTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    // No action — recalculates on next draw
    if (y <= TOPBAR_H) { /* trigger redraw */ }
}
