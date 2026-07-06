#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config/config.h"
#include "config/nvs_config.h"
#include "modules/brightness.h"
#include "modules/wifi_config.h"
#include "ui/theme.h"
#include "ui/theme_color.h"
#include "ui/widgets.h"
#include "touch/touch.h"
#include "modules/location.h"
#include "modules/time_sync.h"
#include "modules/weather.h"
#include "ui/screens/screen_current.h"
#include "ui/screens/screen_hourly.h"
#include "ui/screens/screen_forecast.h"
#include "ui/screens/screen_settings.h"
#include "ui/screens/screen_solar.h"
#include "ui/screens/screen_fireteam.h"
#include "ui/screens/screen_fires.h"
#include "ui/screens/screen_usgs.h"
#include "ui/screens/screen_volcanoes.h"
#include "ui/screens/screen_news.h"
#include "ui/screens/screen_planner.h"
#include "modules/screenshot.h"

static TFT_eSPI tft;

bool g_spriteCapture = false;


// ── App state ─────────────────────────────────────────────────────────────
static int           s_screen         = 0;
static bool          s_needsRedraw    = true;
static bool          s_wifiOk         = false;
static int           s_lastWeatherHour     = -1;   // hour (0-23) of last fetch; -1 = never
static unsigned long s_lastWeatherAttempt  = 0;    // cooldown timer to prevent tight retry loops
static int           s_lastSolarHour       = -1;
static unsigned long s_lastSolarAttempt    = 0;
static int           s_lastFiresHour       = -1;
static unsigned long s_lastFiresAttempt    = 0;
static int           s_lastUsgsHour        = -1;
static unsigned long s_lastUsgsAttempt     = 0;
static int           s_lastNewsHour        = -1;
static unsigned long s_lastNewsAttempt     = 0;
static int           s_lastVolcanoesHour     = -1;
static unsigned long s_lastVolcanoesAttempt  = 0;
static unsigned long s_lastMinute          = 0;
static unsigned long s_lastAutoRotate      = 0;
static unsigned long s_lastTouchMs         = 0;    // sleep timer
static bool         s_backlightOff         = false; // sleep timer
static bool         s_scheduleSleeping     = false; // schedule put backlight to sleep
static unsigned long s_schedGraceUntil     = 0;     // grace period after touch during schedule sleep
static char          s_updateStr[24]  = "Never";
enum RefreshBit : uint8_t {
    REFRESH_FIRES     = 1 << 0,
    REFRESH_USGS      = 1 << 1,
    REFRESH_NEWS      = 1 << 2,
    REFRESH_VOLCANOES = 1 << 3,
    REFRESH_SOLAR     = 1 << 4,
    REFRESH_ALL_DATA  = 0x1F
};
static uint8_t s_refreshQueue = 0;
static bool    s_refreshAllWeather = false;

// ── RGB LED ───────────────────────────────────────────────────────────────
static void ledSet(bool r, bool g, bool b) {
    digitalWrite(LED_R, r ? LOW : HIGH);
    digitalWrite(LED_G, g ? LOW : HIGH);
    digitalWrite(LED_B, b ? LOW : HIGH);
}

// ── Async fetch worker (Core 0) ────────────────────────────────────────────
enum FetchCmd : uint8_t {
    FETCH_NONE = 0,
    FETCH_WEATHER,
    FETCH_WEATHER_LOC,
    FETCH_BOOT_BG,    // pre-fetch all data screens at boot
    FETCH_FIRES,
    FETCH_USGS,
    FETCH_VOLCANOES,
    FETCH_SOLAR,
    FETCH_NEWS
};

static TaskHandle_t      s_fetchTask    = nullptr;
static SemaphoreHandle_t s_dataMutex    = nullptr;
static volatile FetchCmd s_fetchCmd     = FETCH_NONE;
static volatile bool     s_fetchDone    = false;
static volatile bool     s_fetchOk      = false;

// Per-screen completion (worker writes, main loop reads+clears)
static volatile bool s_firesDone = false, s_firesOk = false;
static volatile bool s_usgsDone  = false, s_usgsOk  = false;
static volatile bool s_solarDone = false;
static volatile bool s_volcanoesDone = false, s_volcanoesOk = false;
static volatile bool s_newsDone = false, s_newsOk = false;

static void fetchWorker(void *param) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        FetchCmd cmd = s_fetchCmd;

        switch (cmd) {
            case FETCH_WEATHER: {
                bool ok = weatherFetch(g_location.lat, g_location.lon);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                if (ok) {
                    char t[10]; timeGetShort(t);
                    snprintf(s_updateStr, sizeof(s_updateStr), "Updated %s", t);
                    if (timeIsValid()) {
                        time_t now = time(nullptr);
                        s_lastWeatherHour = localtime(&now)->tm_hour;
                    }
                }
                s_fetchOk = ok;
                s_fetchDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            }
            case FETCH_WEATHER_LOC:
                locationFetch();
                {
                    bool ok = weatherFetch(g_location.lat, g_location.lon);
                    xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                    if (ok) {
                        char t[10]; timeGetShort(t);
                        snprintf(s_updateStr, sizeof(s_updateStr), "Updated %s", t);
                        if (timeIsValid()) {
                            time_t now = time(nullptr);
                            s_lastWeatherHour = localtime(&now)->tm_hour;
                        }
                    }
                    s_fetchOk = ok;
                    s_fetchDone = true;
                    xSemaphoreGive(s_dataMutex);
                }
                break;
            case FETCH_BOOT_BG:
                // Weather first (worker task = WiFiClientSecure works)
                {
                    bool ok = weatherFetch(g_location.lat, g_location.lon);
                    xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                    if (ok) {
                        char t[10]; timeGetShort(t);
                        snprintf(s_updateStr, sizeof(s_updateStr), "Updated %s", t);
                        if (timeIsValid()) {
                            time_t now = time(nullptr);
                            s_lastWeatherHour = localtime(&now)->tm_hour;
                        }
                    }
                    s_fetchOk = ok;
                    s_fetchDone = true;
                    xSemaphoreGive(s_dataMutex);
                }

                g_firesPending  = true;
                g_usgsPending   = true;
                g_solarPending     = true;
                g_newsPending      = true;
                g_volcanoesPending = true;

                firesFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_firesDone = true;
                xSemaphoreGive(s_dataMutex);

                usgsFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_usgsDone = true;
                xSemaphoreGive(s_dataMutex);

                solarFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_solarDone = true;
                xSemaphoreGive(s_dataMutex);

                newsFetch(s_wifiOk, g_location.city);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_newsDone = true;
                xSemaphoreGive(s_dataMutex);

                volcanoesFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_volcanoesDone = true;
                xSemaphoreGive(s_dataMutex);
                break;

            case FETCH_FIRES: {
                bool ok = firesFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_firesOk = ok;
                s_firesDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            }
            case FETCH_USGS: {
                bool ok = usgsFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_usgsOk = ok;
                s_usgsDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            }
            case FETCH_SOLAR:
                solarFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_solarDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            case FETCH_VOLCANOES: {
                bool ok = volcanoesFetch(s_wifiOk);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_volcanoesOk = ok;
                s_volcanoesDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            }
            case FETCH_NEWS: {
                bool ok = newsFetch(s_wifiOk, g_location.city);
                xSemaphoreTake(s_dataMutex, portMAX_DELAY);
                s_newsOk = ok;
                s_newsDone = true;
                xSemaphoreGive(s_dataMutex);
                break;
            }
            default:
                break;
        }

        s_fetchCmd = FETCH_NONE;
        ledSet(false, false, false);
    }
}

static bool workerBusy() { return s_fetchCmd != FETCH_NONE; }

// Weather trigger (hourly — called from loop)
static void triggerFetch(bool includeLocation) {
    if (!s_fetchTask || workerBusy()) return;
    s_fetchDone = false;
    s_fetchCmd  = includeLocation ? FETCH_WEATHER_LOC : FETCH_WEATHER;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

// Screen triggers (called from screen draw functions — must be non-static)
void triggerFiresFetch() {
    if (!s_fetchTask || workerBusy()) return;
    s_firesDone = false;
    s_fetchCmd  = FETCH_FIRES;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

void triggerUsgsFetch() {
    if (!s_fetchTask || workerBusy()) return;
    s_usgsDone = false;
    s_fetchCmd  = FETCH_USGS;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

void triggerSolarFetch() {
    if (!s_fetchTask || workerBusy()) return;
    s_solarDone = false;
    s_fetchCmd  = FETCH_SOLAR;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

void triggerNewsFetch() {
    if (!s_fetchTask || workerBusy()) return;
    s_newsDone = false;
    s_fetchCmd  = FETCH_NEWS;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

void triggerVolcanoesFetch() {
    if (!s_fetchTask || workerBusy()) return;
    s_volcanoesDone = false;
    s_fetchCmd  = FETCH_VOLCANOES;
    ledSet(false, false, true);
    xTaskNotifyGive(s_fetchTask);
}

// ── Splash art — quantum smoke / orbital design ───────────────────────────
static void drawSplashArt(TFT_eSPI &tft) {
    const int cx = 160, cy = 60;

    // 12 dim radiating spokes
    for (int i = 0; i < 12; i++) {
        float a  = i * M_PI / 6.0f;
        int   x1 = cx + (int)(14 * cosf(a));
        int   y1 = cy + (int)(14 * sinf(a));
        int   x2 = cx + (int)(52 * cosf(a));
        int   y2 = cy + (int)(52 * sinf(a));
        if (y2 > 108) y2 = 108;
        tft.drawLine(x1, y1, x2, y2, COL_DIM);
    }

    // Concentric rings: dim inner, themed outer
    tft.drawCircle(cx, cy, 14, COL_DIM);
    tft.drawCircle(cx, cy, 28, COL_DIM);
    tft.drawCircle(cx, cy, 44, g_themeColor);

    // 6 themed accent rays between main spokes
    for (int i = 0; i < 6; i++) {
        float a  = i * M_PI / 3.0f + M_PI / 12.0f;
        int   x1 = cx + (int)(28 * cosf(a));
        int   y1 = cy + (int)(28 * sinf(a));
        int   x2 = cx + (int)(44 * cosf(a));
        int   y2 = cy + (int)(44 * sinf(a));
        if (y2 > 108) y2 = 108;
        tft.drawLine(x1, y1, x2, y2, g_themeColor);
    }

    // Outer particles at r=48-58, clipped to art area
    for (int i = 0; i < 18; i++) {
        float a  = i * M_PI / 9.0f;
        float r  = 48.0f + (i % 3) * 5.0f;
        int   px = cx + (int)(r * cosf(a));
        int   py = cy + (int)(r * sinf(a));
        if (py > 108) continue;
        tft.fillCircle(px, py, 1, (i % 3 == 0) ? g_themeColor : COL_DIM);
    }

    // Inner nebula scatter dots
    static const int8_t nebX[] = { -8,  6, -12, 10, -5,  9, -4,  7 };
    static const int8_t nebY[] = { -10, -8,   3,  6, 15, -15, -20, 18 };
    for (int i = 0; i < 8; i++)
        tft.fillCircle(cx + nebX[i], cy + nebY[i], 1, COL_DIM);

    // Center glow
    tft.fillCircle(cx, cy, 7, g_themeColor);
    tft.fillCircle(cx, cy, 3, COL_WHITE);
}

// ── Splash screen ─────────────────────────────────────────────────────────
static void showSplash(const char *msg) {
    tft.fillScreen(COL_BG);
    drawSplashArt(tft);

    int tw;

    tft.setTextFont(FONT_LG);
    tft.setTextColor(g_themeColor, COL_BG);
    tw = tft.textWidth("xXMayDayXx");
    tft.setCursor((SCREEN_W - tw) / 2, 118);
    tft.print("xXMayDayXx");

    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tw = tft.textWidth("xXCYD-Weather-StationXx");
    tft.setCursor((SCREEN_W - tw) / 2, 152);
    tft.print("xXCYD-Weather-StationXx");

    tft.setTextColor(g_themeColor, COL_BG);
    tw = tft.textWidth("xXQuantum-SmokeXx");
    tft.setCursor((SCREEN_W - tw) / 2, 177);
    tft.print("xXQuantum-SmokeXx");

    if (msg && msg[0]) {
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, 210);
        tft.print(msg);
    }
}

// ── WiFi ──────────────────────────────────────────────────────────────────
static void connectWifi() {
    showSplash("Connecting to WiFi...");
    char ssid[64], pass[64];
    wifiGetSSID(ssid, sizeof(ssid));
    wifiGetPass(pass, sizeof(pass));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(100);
    s_wifiOk = (WiFi.status() == WL_CONNECTED);
    if (!s_wifiOk) showSplash("WiFi failed!");
}

// ── Weather fetch (sync — used only during boot before worker task starts) ──
static void ensureLocation() {
    if (!s_wifiOk) return;
    if (!g_location.valid) {
        g_location.lat       = 39.8283f;
        g_location.lon       = -98.5795f;
        g_location.utcOffset = -18000;
        g_location.valid     = true;
        strncpy(g_location.city,   "Kansas City", sizeof(g_location.city) - 1);
        strncpy(g_location.region, "Missouri",    sizeof(g_location.region) - 1);
        strncpy(g_location.tz,     "America/Chicago", sizeof(g_location.tz) - 1);
    }
}

// Screens: 0=Now, 1=Hourly, 2=5-Day, 3=Solar, 4=Fireteam, 5=Fires, 6=USGS, 7=Volcanoes, 8=News, 9=Planner, 10=Settings
static void gotoScreen(int n) {
    s_screen         = constrain(n, 0, 10);
    s_lastAutoRotate = millis();
    s_needsRedraw    = true;
}

static void redrawTo(TFT_eSPI &target) {
    switch (s_screen) {
        case 0: screenCurrentDraw(target, s_wifiOk);  break;
        case 1: screenHourlyDraw(target, s_wifiOk);   break;
        case 2: screenForecastDraw(target, s_wifiOk); break;
        case 3: screenSolarDraw(target, s_wifiOk);    break;
        case 4: screenFireteamDraw(target, s_wifiOk); break;
        case 5: screenFiresDraw(target, s_wifiOk);    break;
        case 6: screenUsgsDraw(target, s_wifiOk);     break;
        case 7: screenVolcanoesDraw(target, s_wifiOk); break;
        case 8: screenNewsDraw(target, s_wifiOk);     break;
        case 9: screenPlannerDraw(target, s_wifiOk);  break;
        case 10: screenSettingsDraw(target, s_wifiOk); break;
    }
}

static void redraw() {
    redrawTo(tft);
    s_needsRedraw = false;
}

// ── First-boot rotation calibration (2USB only) ──────────────────────────────
// Some 2USB panels are physically rotated — tap to cycle rotation, hold 2s to confirm.
// Runs once; persisted via NVS key "rot_cal". Only for CYD_USB_VERSION == 2.

static int  s_rotation = 1;   // default landscape, overridden by NVS or calibration

// ── ILI9341 MADCTL configuration for 2USB landscape displays ──────────────
// Different CYD boards mount the LCD glass in different orientations.  We
// combine two bits to cover all variants:
//   MV (bit 5): row/column swap — fixes 90° rotation on portrait-native glass
//   MY (bit 7): Y-axis mirror — fixes upside-down display
// Four combos cycle via calibration; the chosen value is stored in NVS.
static uint8_t s_madctl = 0x80;   // NVS-backed MADCTL value

static void applyRotation() {
#if CYD_USB_VERSION == 2
    tft.setRotation(1);                              // landscape dimensions
    tft.writecommand(TFT_MADCTL);                    // override MADCTL set by setRotation
    tft.writedata(s_madctl);
#else
    tft.setRotation(s_rotation);
#endif
}

// Build the MADCTL value for a given combo index (0-3).
// 0: MV=1, MY=0 → 0x28 (swap, no mirror  — portrait glass)
// 1: MV=1, MY=1 → 0xA8 (swap + mirror)
// 2: MV=0, MY=0 → 0x00 (no swap, no mirror — landscape glass)
// 3: MV=0, MY=1 → 0x80 (no swap, mirror   — landscape glass flipped)
static uint8_t madctlForCombo(int idx) {
    switch (idx & 3) {
        case 0:  return TFT_MAD_MV | TFT_MAD_BGR;           // 0x28
        case 1:  return TFT_MAD_MV | TFT_MAD_MY | TFT_MAD_BGR; // 0xA8
        case 2:  return 0x00;                               // 0x00
        default: return TFT_MAD_MY;                          // 0x80
    }
}

// ── Calibration version ─────────────────────────────────────────────────────
// Single versioned key replaces the old individual-key approach.  If cal_ver
// isn't CURRENT_CAL_VER (e.g. after an upgrade or fresh flash), all calibration
// screens re-run.  This avoids the v1.1.8 bug where individual keys were
// silently filled by the upgrade-path code, suppressing the actual screens.
#define CURRENT_CAL_VER  2

// ── First-boot display calibration (2USB only) ────────────────────────────
// Cycles through the 4 MADCTL combinations so the user can find the correct
// one.  Shows a large asymmetric pattern that makes the orientation obvious.
// Serial M cycles MADCTL combos; T cycles touch rotation.
static void displayCalibrate() {
#if CYD_USB_VERSION == 2
    if (nvsGetInt("cal_ver", -1) >= CURRENT_CAL_VER) {
        s_madctl = (uint8_t)nvsGetInt("madctl", 0x80);
        return;
    }

    s_madctl = madctlForCombo(0);  // start at combo 0
    digitalWrite(TFT_BL, HIGH);

    auto drawDisplayCal = [&]() {
        tft.fillScreen(COL_BG);
        applyRotation();
        tft.fillScreen(COL_BG);

        // ── Distinctive corner markers ──
        // Top-left: amber filled triangle pointing down-right
        tft.fillTriangle(2, 2, 60, 2, 2, 60, COL_AMBER);
        tft.fillTriangle(4, 4, 56, 4, 4, 56, COL_BG);
        tft.fillTriangle(2, 2, 60, 2, 2, 60, COL_AMBER);

        // Top-right: colored "L" bracket
        tft.fillRect(SCREEN_W - 50, 2, 48, 8, g_themeColor);
        tft.fillRect(SCREEN_W - 8, 2, 6, 48, g_themeColor);

        // Bottom-left: amber ring
        tft.fillCircle(24, SCREEN_H - 24, 20, COL_AMBER);
        tft.fillCircle(24, SCREEN_H - 24, 16, COL_BG);
        tft.fillCircle(24, SCREEN_H - 24, 20, COL_AMBER);

        // Bottom-right: crosshair + circle
        tft.drawLine(SCREEN_W - 40, SCREEN_H - 24, SCREEN_W - 8, SCREEN_H - 24, g_themeColor);
        tft.drawLine(SCREEN_W - 24, SCREEN_H - 40, SCREEN_W - 24, SCREEN_H - 8, g_themeColor);
        tft.drawCircle(SCREEN_W - 24, SCREEN_H - 24, 14, g_themeColor);

        // Center: "T" orientation letter
        tft.fillRect(SCREEN_W / 2 - 16, SCREEN_H / 2 - 24, 32, 6, COL_WHITE);
        tft.fillRect(SCREEN_W / 2 - 4, SCREEN_H / 2 - 24, 8, 48, COL_WHITE);

        // ── Combo number and description ──
        int idx;
        if      (s_madctl == (TFT_MAD_MV | TFT_MAD_BGR))             idx = 0;
        else if (s_madctl == (TFT_MAD_MV | TFT_MAD_MY | TFT_MAD_BGR)) idx = 1;
        else if (s_madctl == 0x00)                                    idx = 2;
        else                                                          idx = 3;

        tft.setTextFont(FONT_LG);
        tft.setTextColor(g_themeColor, COL_BG);
        char buf[16]; snprintf(buf, sizeof(buf), "MODE %d", idx);
        int tw = tft.textWidth(buf);
        tft.setCursor((SCREEN_W - tw) / 2, 68);
        tft.print(buf);

        // Tap instruction
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        const char *msg = "Tap to change";
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H - 72);
        tft.print(msg);

        // Hold instruction
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        msg = "Hold 2s to confirm";
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H - 52);
        tft.print(msg);
    };

    drawDisplayCal();

    {
        unsigned long holdStart = 0;
        bool wasTouched = false;
        int  curCombo = 0;

        while (true) {
            bool nowTouched = touchIsHeld();

            if (nowTouched && !wasTouched) {
                holdStart = millis();
            } else if (!nowTouched && wasTouched && holdStart > 0) {
                if (millis() - holdStart < 1200) {
                    curCombo = (curCombo + 1) & 3;
                    s_madctl = madctlForCombo(curCombo);
                    drawDisplayCal();
                }
            }

            if (nowTouched && wasTouched && holdStart > 0) {
                if (millis() - holdStart >= 2000) break;
            }

            wasTouched = nowTouched;
            delay(30);
        }

        while (touchIsHeld()) { delay(30); }
        delay(200);
    }

    nvsPutInt("madctl", s_madctl);
    // cal_ver is written by touchCalibrate() below
#endif
}

// ── First-boot touch calibration ─────────────────────────────────────────────
// 2USB boards ship with digitizers in one of four orientations. This cycles
// through all four XPT2046 rotations (0-3) so the user can find the one where
// the touch cursor follows their finger correctly.
// Tap to cycle; hold 2s to confirm. Runs once (cal_ver guard shared with
// rotation/mirror).  Serial 'T' and Settings → Touch Flip re-trigger.
static void touchCalibrate() {
#if CYD_USB_VERSION == 2
    if (nvsGetInt("cal_ver", -1) >= CURRENT_CAL_VER) return;

    digitalWrite(TFT_BL, HIGH);

    // ── Draw static background (rotation number, instructions, corner targets) ──
    auto drawStatic = []() {
        tft.fillScreen(COL_BG);

        // Rotation number dead center
        tft.setTextFont(FONT_LG);
        tft.setTextColor(g_themeColor, COL_BG);
        char buf[4]; snprintf(buf, sizeof(buf), "%d", touchGetRotation());
        int tw = tft.textWidth(buf);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 - 40);
        tft.print(buf);

        // Primary instruction
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        const char *msg = "Tap to cycle touch";
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2);
        tft.print(msg);

        // Secondary instruction
        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        msg = "Hold 2s to confirm";
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 + 30);
        tft.print(msg);

        // Four corner crosshair targets — touch each to verify cursor alignment
        const int CX = 14, CY = 14, CS = 18;
        uint16_t tc = COL_DIM;
        tft.drawRect(CX, CY, CS, CS, tc);
        tft.drawLine(CX, CY, CX + CS, CY + CS, tc);
        tft.drawLine(CX, CY + CS, CX + CS, CY, tc);

        tft.drawRect(SCREEN_W - CX - CS, CY, CS, CS, tc);
        tft.drawLine(SCREEN_W - CX - CS, CY, SCREEN_W - CX, CY + CS, tc);
        tft.drawLine(SCREEN_W - CX - CS, CY + CS, SCREEN_W - CX, CY, tc);

        tft.drawRect(CX, SCREEN_H - CY - CS, CS, CS, tc);
        tft.drawLine(CX, SCREEN_H - CY, CX + CS, SCREEN_H - CY - CS, tc);
        tft.drawLine(CX, SCREEN_H - CY - CS, CX + CS, SCREEN_H - CY, tc);

        tft.drawRect(SCREEN_W - CX - CS, SCREEN_H - CY - CS, CS, CS, tc);
        tft.drawLine(SCREEN_W - CX, SCREEN_H - CY, SCREEN_W - CX - CS, SCREEN_H - CY - CS, tc);
        tft.drawLine(SCREEN_W - CX - CS, SCREEN_H - CY, SCREEN_W - CX, SCREEN_H - CY - CS, tc);
    };

    drawStatic();

    unsigned long holdStart = 0;
    bool wasTouched = false;
    int  curX = -1, curY = -1;
    int  lastX = -1, lastY = -1;
    bool dirty = false;

    while (true) {
        int16_t tx, ty;
        bool nowTouched = touchIsHeld(&tx, &ty);

        if (nowTouched) {
            curX = tx; curY = ty;
        }

        // ── Touch-down ──
        if (nowTouched && !wasTouched) {
            holdStart = millis();
            lastX = curX; lastY = curY;
            dirty = true;                // draw cursor
        }
        // ── Released ──
        else if (!nowTouched && wasTouched && holdStart > 0) {
            if (millis() - holdStart < 1200) {
                touchSetRotation((touchGetRotation() + 1) % 4);
                drawStatic();
                // Re-draw cursor if finger is back down (edge: retap within one frame)
            }
            // Erase cursor
            tft.fillCircle(lastX, lastY, 7, COL_BG);
            lastX = lastY = -1;
            dirty = false;
        }
        // ── Hold-to-confirm ──
        else if (nowTouched && wasTouched && holdStart > 0) {
            if (millis() - holdStart >= 2000) {
                // Erase cursor before we leave
                if (lastX >= 0) tft.fillCircle(lastX, lastY, 7, COL_BG);
                break;
            }
        }

        // ── Update cursor position ──
        if (nowTouched && dirty && (curX != lastX || curY != lastY)) {
            // Erase old position
            if (lastX >= 0) tft.fillCircle(lastX, lastY, 7, COL_BG);
            // Draw new position
            tft.fillCircle(curX, curY, 6, COL_AMBER);
            tft.drawCircle(curX, curY, 6, COL_WHITE);
            lastX = curX; lastY = curY;
        }

        wasTouched = nowTouched;
        delay(30);
    }

    // Wait for finger to lift
    while (touchIsHeld()) { delay(30); }
    delay(200);

    nvsPutInt("cal_ver", CURRENT_CAL_VER);  // marks ALL calibrations complete
    nvsPutInt("touch_cal", 1);              // keep old key for backwards compat
#endif
}

// ─────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
    ledSet(false, false, false);

    nvsInit();
    wifiConfigLoad();   // reads wifi.txt from SD if present, saves to NVS
    themeColorInit();

    // TFT_eSPI init
    tft.init();

    // ── Display rotation ─────────────────────────────────────────────────
    // ESP32-32E (1-USB): standard landscape → rotation 1
    // 2-USB:              NVS-backed rotation (calibrated on first boot)
    applyRotation();

    tft.fillScreen(COL_BG);
    brightnessInit();

    // Touch on VSPI (separate from TFT_eSPI's HSPI bus)
    touchInit();

    // First-boot calibrations — only on 2USB, only once each
    displayCalibrate();
    applyRotation();   // re-apply in case calibration changed it
    touchCalibrate();

    showSplash("Starting up...");
    ledSet(false, true, false);

    connectWifi();
    s_wifiOk = (WiFi.status() == WL_CONNECTED);

    s_dataMutex = xSemaphoreCreateMutex();

    if (s_wifiOk) {
        showSplash("Getting location...");
        locationLoad();
        if (!g_location.valid) locationFetch();

        showSplash("Syncing time...");
        timeSyncInit(g_location.valid ? g_location.utcOffset : 0);
        for (int i = 0; i < 30 && !timeIsValid(); i++) delay(100);   // 3 s max

        ensureLocation();
        screenshotInit(tft, redrawTo);
    }

    // Launch async fetch worker on Core 0
    xTaskCreatePinnedToCore(fetchWorker, "fetch", 16384, nullptr, 1, &s_fetchTask, 0);

    // Kick off background fetches — data screens + weather (worker task = TLS works)
    if (s_wifiOk) {
        s_fetchCmd = FETCH_BOOT_BG;
        xTaskNotifyGive(s_fetchTask);

        showSplash("Fetching weather...");
        for (int i = 0; i < 160 && !s_fetchDone && s_wifiOk; i++) delay(50);  // 8 s

        if (s_wifiOk) showSplash("Fetching fires...");
        for (int i = 0; i < 60 && !s_firesDone && s_wifiOk; i++) delay(50);

        if (s_wifiOk) showSplash("Fetching USGS...");
        for (int i = 0; i < 60 && !s_usgsDone && s_wifiOk; i++) delay(50);

        if (s_wifiOk) showSplash("Fetching solar...");
        for (int i = 0; i < 60 && !s_solarDone && s_wifiOk; i++) delay(50);

        if (s_wifiOk) showSplash("Fetching news...");
        for (int i = 0; i < 60 && !s_newsDone && s_wifiOk; i++) delay(50);

        if (s_wifiOk) showSplash("Fetching volcanoes...");
        for (int i = 0; i < 60 && !s_volcanoesDone && s_wifiOk; i++) delay(50);
    }

    ledSet(false, false, false);
    s_lastTouchMs = millis();      // prevent immediate sleep timer trigger
    s_needsRedraw = true;
    Serial.println("READY");
}

// ─────────────────────────────────────────────────────────────────────────
void loop() {
    // Brightness update (auto=LDR, fixed=no-op)
    static unsigned long lastLdr = 0;
    if (millis() - lastLdr > 5000) {
        if (!s_backlightOff) brightnessAutoUpdate();
        lastLdr = millis();
    }

    // WiFi health check — update status and attempt reconnect if link is down.
    // s_wifiOk was previously set once at boot and never updated, so a mid-run
    // disconnection would cause silent fetch failures forever.
    {
        static unsigned long s_lastWifiCheck = 0;
        if (millis() - s_lastWifiCheck > 30000) {
            wl_status_t st = WiFi.status();
            if (st != WL_CONNECTED && s_wifiOk) {
                // Connection dropped — try to bring it back
                WiFi.reconnect();
            }
            s_wifiOk = (st == WL_CONNECTED);
            s_lastWifiCheck = millis();
        }
    }

    // Weather and data refresh scheduler. Every due source is queued, then a
    // single job is dispatched when the Core 0 worker is free.
    if (s_wifiOk && timeIsValid()) {
        time_t now = time(nullptr);
        int curHour = localtime(&now)->tm_hour;

        // Periodic NTP re-sync — ESP32 clocks drift over hours/days.
        // A quiet re-sync every 6 h keeps localtime() accurate so the
        // hour-change trigger fires at the right wall-clock hour.
        {
            static unsigned long s_lastNtpSync = 0;
            if (millis() - s_lastNtpSync > 21600000UL) {  // 6 h
                configTime(g_utcOffsetSec, 0, "pool.ntp.org", "time.cloudflare.com");
                s_lastNtpSync = millis();
            }
        }

        bool weatherDue   = curHour != s_lastWeatherHour;
        bool weatherStale = (g_weatherUpdatedEpoch > 0) &&
                            (now - (time_t)g_weatherUpdatedEpoch > 7200); // 2 h
        if (!workerBusy() && (s_refreshAllWeather ||
            ((weatherDue || weatherStale) && millis() - s_lastWeatherAttempt > 60000))) {
            bool includeLocation = s_refreshAllWeather;
            s_refreshAllWeather = false;
            s_lastWeatherAttempt = millis();
            triggerFetch(includeLocation);
        }

        if (!g_firesPending && !(s_refreshQueue & REFRESH_FIRES) && curHour != s_lastFiresHour && millis() - s_lastFiresAttempt > 60000) s_refreshQueue |= REFRESH_FIRES;
        if (!g_usgsPending && !(s_refreshQueue & REFRESH_USGS) && curHour != s_lastUsgsHour && millis() - s_lastUsgsAttempt > 60000) s_refreshQueue |= REFRESH_USGS;
        if (!g_newsPending && !(s_refreshQueue & REFRESH_NEWS) && curHour != s_lastNewsHour && millis() - s_lastNewsAttempt > 60000) s_refreshQueue |= REFRESH_NEWS;
        if (!g_volcanoesPending && !(s_refreshQueue & REFRESH_VOLCANOES) && curHour != s_lastVolcanoesHour && millis() - s_lastVolcanoesAttempt > 60000) s_refreshQueue |= REFRESH_VOLCANOES;
        if (!g_solarPending && !(s_refreshQueue & REFRESH_SOLAR) && curHour != s_lastSolarHour && millis() - s_lastSolarAttempt > 60000) s_refreshQueue |= REFRESH_SOLAR;
    }

    if (s_wifiOk && !workerBusy() && s_refreshQueue) {
        if (s_refreshQueue & REFRESH_FIRES) {
            s_refreshQueue &= ~REFRESH_FIRES;
            g_firesPending = true; s_lastFiresAttempt = millis(); triggerFiresFetch();
        } else if (s_refreshQueue & REFRESH_USGS) {
            s_refreshQueue &= ~REFRESH_USGS;
            g_usgsPending = true; s_lastUsgsAttempt = millis(); triggerUsgsFetch();
        } else if (s_refreshQueue & REFRESH_NEWS) {
            s_refreshQueue &= ~REFRESH_NEWS;
            g_newsPending = true; s_lastNewsAttempt = millis(); triggerNewsFetch();
        } else if (s_refreshQueue & REFRESH_VOLCANOES) {
            s_refreshQueue &= ~REFRESH_VOLCANOES;
            g_volcanoesPending = true; s_lastVolcanoesAttempt = millis(); triggerVolcanoesFetch();
        } else if (s_refreshQueue & REFRESH_SOLAR) {
            s_refreshQueue &= ~REFRESH_SOLAR;
            g_solarPending = true; s_lastSolarAttempt = millis(); triggerSolarFetch();
        }
    }
    // Weather fetch completion
    if (s_fetchDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        bool ok = s_fetchOk;
        s_fetchDone = false;
        xSemaphoreGive(s_dataMutex);
        if (!ok) s_lastWeatherAttempt = millis();
        s_needsRedraw = true;
    }

    // Fires fetch completion
    if (s_firesDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        bool ok = s_firesOk;
        s_firesDone = false;
        g_firesPending = false;
        xSemaphoreGive(s_dataMutex);
        if (ok && timeIsValid()) { time_t now = time(nullptr); s_lastFiresHour = localtime(&now)->tm_hour; }
        else s_lastFiresHour = -1;
        if (s_screen == 5) s_needsRedraw = true;
    }

    // USGS fetch completion
    if (s_usgsDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        bool ok = s_usgsOk;
        s_usgsDone = false;
        g_usgsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (ok && timeIsValid()) { time_t now = time(nullptr); s_lastUsgsHour = localtime(&now)->tm_hour; }
        else s_lastUsgsHour = -1;
        if (s_screen == 6) s_needsRedraw = true;
    }

    // Volcanoes fetch completion
    if (s_volcanoesDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        bool ok = s_volcanoesOk;
        s_volcanoesDone = false;
        g_volcanoesPending = false;
        xSemaphoreGive(s_dataMutex);
        if (ok && timeIsValid()) { time_t now = time(nullptr); s_lastVolcanoesHour = localtime(&now)->tm_hour; }
        else s_lastVolcanoesHour = -1;
        if (s_screen == 7) s_needsRedraw = true;
    }

    // Solar fetch completion
    if (s_solarDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_solarDone = false;
        g_solarPending = false;
        xSemaphoreGive(s_dataMutex);
        if (timeIsValid()) {
            time_t now = time(nullptr);
            s_lastSolarHour = localtime(&now)->tm_hour;
        }
        if (s_screen == 3) s_needsRedraw = true;
    }

    // News fetch completion
    if (s_newsDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        bool ok = s_newsOk;
        s_newsDone = false;
        g_newsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (ok && timeIsValid()) { time_t now = time(nullptr); s_lastNewsHour = localtime(&now)->tm_hour; }
        else s_lastNewsHour = -1;
        if (s_screen == 8) s_needsRedraw = true;
    }

    // Clock tick — only update the time text, not a full redraw
    if (millis() - s_lastMinute > 60000) {
        if (s_screen <= 2 || s_screen == 4 || s_screen >= 8) {
            char timeStr[10]; timeGetShort(timeStr);
            static const char* labels[] = {"NOW","HOURLY","5-DAY","SOLAR","FIRETEAM","FIRES","USGS","VOLCANOES","NEWS","ALMANAC","SETTINGS"};
            drawTopbarTime(tft, timeStr, labels[s_screen]);
        }
        s_lastMinute = millis();
    }

    // Auto-rotate through enabled pages
    if (screenSettingsGetAutoRotate() &&
        millis() - s_lastAutoRotate > screenSettingsGetAutoRotateMs()) {
        int start = (s_screen == 10) ? 9 : s_screen;
        int next = screenSettingsGetNextRotatePage(start);
        if (next >= 0) gotoScreen(next);
    }

    // Touch
    TouchEvent evt = touchPoll();
    if (evt.swipe == SwipeDir::Left) {
        gotoScreen(s_screen + 1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Right) {
        gotoScreen(s_screen - 1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 5) {
        screenFiresSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 5) {
        screenFiresSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 6) {
        screenUsgsSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 6) {
        screenUsgsSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 7) {
        screenVolcanoesSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 7) {
        screenVolcanoesSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 8) {
        screenNewsSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 8) {
        screenNewsSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.tap == TapEvent::Tap) {
        int tx = evt.tapX, ty = evt.tapY;
        int botY = SCREEN_H - BOTBAR_H;
        if (ty >= botY) {
            if (tx < 50)               gotoScreen(s_screen - 1);
            else if (tx > SCREEN_W - 50) gotoScreen(s_screen + 1);
        }
        // Top bar tap → refresh (weather screens 0-2,6; data screens handle their own)
        if (ty < TOPBAR_H && s_wifiOk && !workerBusy() && (s_screen <= 2 || s_screen == 4 || s_screen == 9)) {
            showSplash("Refreshing...");
            triggerFetch(true);
        }
        if (s_screen == 3) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenSolarTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 5) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenFiresTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 6) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenUsgsTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 7) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenVolcanoesTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 8) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenNewsTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 9) {
            screenPlannerTap(tft, tx, ty, s_wifiOk);
            s_needsRedraw = true;
        } else if (s_screen == 10) {
            bool changed = screenSettingsTap(tft, tx, ty);
            if (changed) s_needsRedraw = true;
            if (screenSettingsRefreshTapped()) {
                showSplash("Refreshing...");
                s_refreshAllWeather = true;
                s_refreshQueue |= REFRESH_ALL_DATA;
            }
        }
    }

    // ── Sleep timer — backlight off on inactivity ──────────────────────────
    if (evt.swipe != SwipeDir::None || evt.tap != TapEvent::None) {
        s_lastTouchMs = millis();
        if (s_backlightOff) {
            s_backlightOff = false;
            brightnessRestore();
            if (s_scheduleSleeping) {
                s_schedGraceUntil = millis() + 30000;  // 30s grace before re-sleep
            }
        }
    }

    int slpSecs = screenSettingsGetSleepTimerSecs();
    if (slpSecs > 0 && !g_invert && !s_backlightOff) {
        if (millis() - s_lastTouchMs >= (unsigned long)slpSecs * 1000UL) {
            s_backlightOff = true;
            brightnessOff();
        }
    }

    // ── Schedule-based sleep ──────────────────────────────────────────────────
    // Shuts off backlight during configured daily sleep window (e.g. 10PM–7AM).
    // Independent of the inactivity timer above — touch grants a 30 s grace
    // period before the schedule re-enforces.
    {
        bool schedEnabled = screenSettingsGetScheduleEnabled();

        // Schedule was disabled while sleeping → wake up now
        if (!schedEnabled && s_scheduleSleeping) {
            s_scheduleSleeping = false;
            if (s_backlightOff) {
                s_backlightOff = false;
                brightnessRestore();
                s_needsRedraw = true;
            }
        }

        if (schedEnabled && !g_invert && timeIsValid()) {
            time_t now = time(nullptr);
            int curHour = localtime(&now)->tm_hour;
            int sleepHr  = screenSettingsGetSleepHour();
            int wakeHr   = screenSettingsGetWakeHour();

            bool inWindow;
            if (sleepHr < wakeHr) {
                // Same-day window, e.g. midnight–7AM
                inWindow = (curHour >= sleepHr && curHour < wakeHr);
            } else {
                // Overnight window, e.g. 10PM–7AM
                inWindow = (curHour >= sleepHr || curHour < wakeHr);
            }

            if (inWindow && !s_backlightOff && millis() > s_schedGraceUntil) {
                s_scheduleSleeping = true;
                s_backlightOff = true;
                brightnessOff();
            } else if (!inWindow && s_scheduleSleeping) {
                s_scheduleSleeping = false;
                if (s_backlightOff) {
                    s_backlightOff = false;
                    brightnessRestore();
                    s_needsRedraw = true;
                }
            }
        }
    }

    screenshotLoop();
    if (screenshotNeedsRedraw()) s_needsRedraw = true;

    // Serial commands: '0'-'9' = screens 0-9, 'A' = Settings, 'R' = ready, 'S' = capture
    if (Serial.available()) {
        int cmd = Serial.read();
        if (cmd == 'R' || cmd == 'r') {
            Serial.println("READY");
        }
        if (cmd == 'M' || cmd == 'm') {
            // Cycle through MADCTL combos 0→1→2→3→0
            int cur = 3;
            if      (s_madctl == (TFT_MAD_MV | TFT_MAD_BGR))                cur = 0;
            else if (s_madctl == (TFT_MAD_MV | TFT_MAD_MY | TFT_MAD_BGR))  cur = 1;
            else if (s_madctl == 0x00)                                       cur = 2;
            cur = (cur + 1) & 3;
            s_madctl = madctlForCombo(cur);
            nvsPutInt("madctl", s_madctl);
            nvsPutInt("cal_ver", CURRENT_CAL_VER);   // manual override counts as calibrated
            applyRotation();
            Serial.print("MADCTL_MODE:");
            Serial.println(cur);
            s_needsRedraw = true;
        }
        if (cmd == 'T' || cmd == 't') {
            int rot = (touchGetRotation() + 1) % 4;
            touchSetRotation(rot);
            nvsPutInt("touch_cal", 1);   // skip first-boot calibration from now on
            Serial.print("TOUCH_ROT:");
            Serial.println(rot);
            s_needsRedraw = true;
        }
        if (cmd >= '0' && cmd <= '9') {
            int n = cmd - '0';
            if (s_backlightOff) { s_backlightOff = false; brightnessRestore(); }
            gotoScreen(n);
            redraw();
        } else if (cmd == 'A' || cmd == 'a') {
            if (s_backlightOff) { s_backlightOff = false; brightnessRestore(); }
            gotoScreen(10);
            redraw();
        }
        if (cmd == 'S' || cmd == 's') {
            // Force backlight on and redraw so GRAM is fresh for capture
            bool blWasOff = s_backlightOff;
            if (blWasOff) { s_backlightOff = false; brightnessRestore(); }

            // Try full-screen sprite first (fastest — one render, one send)
            TFT_eSprite spr(&tft);
            spr.setColorDepth(8);
            uint8_t *fb = (uint8_t*)spr.createSprite(SCREEN_W, SCREEN_H);
            if (fb) {
                bool oldCapture = g_spriteCapture;
                g_spriteCapture = false;
                redrawTo(spr);
                g_spriteCapture = oldCapture;
                Serial.print("RGB332:");
                Serial.write(fb, SCREEN_W * SCREEN_H);
                Serial.flush();
                spr.deleteSprite();
            } else {
                // Sprite failed — render to TFT, then read back line-by-line
                // using readRect (one SPI transaction per line vs per pixel)
                redraw();
                uint16_t lineBuf[SCREEN_W];   // 640 bytes on stack
                Serial.print("RGB332:");
                for (int y = 0; y < SCREEN_H; y++) {
                    tft.readRect(0, y, SCREEN_W, 1, lineBuf);
                    for (int x = 0; x < SCREEN_W; x++) {
                        uint16_t c = lineBuf[x];
                        uint8_t b = ((c >> 13) & 0x07) << 5
                                  | ((c >>  8) & 0x07) << 2
                                  | ((c >>  3) & 0x03);
                        Serial.write(b);
                    }
                }
                Serial.flush();
            }

            // Restore backlight if it was off before capture
            if (blWasOff) { s_backlightOff = true; brightnessOff(); }
        }
    }

    // Skip redraw while worker writes data globals on Core 0
    if (s_needsRedraw && !workerBusy()) redraw();
    delay(20);
}
