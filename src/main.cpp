#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>

#include "config/config.h"
#include "config/nvs_config.h"
#include "modules/brightness.h"
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

static TFT_eSPI tft;

// ── App state ─────────────────────────────────────────────────────────────
static int           s_screen      = 0;
static bool          s_needsRedraw = true;
static bool          s_wifiOk      = false;
static unsigned long s_lastWeather = 0;
static unsigned long s_lastMinute  = 0;
static char          s_updateStr[24] = "Never";

// ── RGB LED ───────────────────────────────────────────────────────────────
static void ledSet(bool r, bool g, bool b) {
    digitalWrite(LED_R, r ? LOW : HIGH);
    digitalWrite(LED_G, g ? LOW : HIGH);
    digitalWrite(LED_B, b ? LOW : HIGH);
}

// ── Splash art — quantum smoke / orbital design ───────────────────────────
static void drawSplashArt(TFT_eSPI &tft) {
    const int cx = 160, cy = 52;

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
    tw = tft.textWidth("xXCYD-WeatherXx");
    tft.setCursor((SCREEN_W - tw) / 2, 152);
    tft.print("xXCYD-WeatherXx");

    tft.setTextColor(g_themeColor, COL_BG);
    tw = tft.textWidth("xXStationXx");
    tft.setCursor((SCREEN_W - tw) / 2, 172);
    tft.print("xXStationXx");

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
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(300);
    s_wifiOk = (WiFi.status() == WL_CONNECTED);
    if (!s_wifiOk) showSplash("WiFi failed!");
}

// ── Weather fetch ─────────────────────────────────────────────────────────
static void fetchWeather() {
    if (!s_wifiOk) return;
    if (!g_location.valid) { showSplash("Location failed.\nCheck serial monitor."); delay(3000); return; }
    ledSet(false, false, true);
    if (weatherFetch(g_location.lat, g_location.lon)) {
        timeSyncInit(g_utcOffsetSec);
        char t[10]; timeGetShort(t);
        snprintf(s_updateStr, sizeof(s_updateStr), "Updated %s", t);
    }
    ledSet(false, false, false);
    s_lastWeather = millis();
    s_needsRedraw = true;
}

// Screens: 0=Now, 1=Hourly, 2=5-Day, 3=Settings
static void gotoScreen(int n) {
    s_screen = constrain(n, 0, 3);
    s_needsRedraw = true;
}

static void redraw() {
    switch (s_screen) {
        case 0: screenCurrentDraw(tft, s_wifiOk);  break;
        case 1: screenHourlyDraw(tft, s_wifiOk);   break;
        case 2: screenForecastDraw(tft, s_wifiOk); break;
        case 3: screenSettingsDraw(tft, s_wifiOk); break;
    }
    s_needsRedraw = false;
}

// ─────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
    ledSet(false, false, false);

    nvsInit();
    themeColorInit();

    // TFT_eSPI init
    tft.init();
    tft.setRotation(1);   // landscape; try 3 if upside-down
    tft.fillScreen(COL_BG);
    brightnessInit();

    // Touch on VSPI (separate from TFT_eSPI's HSPI bus)
    touchInit();

    showSplash("Starting up...");
    ledSet(false, true, false);

    connectWifi();
    s_wifiOk = (WiFi.status() == WL_CONNECTED);

    if (s_wifiOk) {
        showSplash("Getting location...");
        locationLoad();
        // Always fetch fresh — NVS cache may have stale/missing offset
        locationFetch();

        showSplash("Syncing time...");
        // Use UTC offset from ip-api immediately — correct time before weather loads
        timeSyncInit(g_location.valid ? g_location.utcOffset : 0);
        for (int i = 0; i < 50 && !timeIsValid(); i++) delay(200);

        showSplash("Fetching weather...");
        fetchWeather();
    }

    ledSet(false, false, false);
    s_needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────────────────
void loop() {
    // Brightness update (auto=LDR, fixed=no-op)
    static unsigned long lastLdr = 0;
    if (millis() - lastLdr > 5000) {
        brightnessAutoUpdate();
        lastLdr = millis();
    }

    // Weather refresh
    if (s_wifiOk && millis() - s_lastWeather > WEATHER_REFRESH_MS) fetchWeather();

    // Clock tick
    if (millis() - s_lastMinute > 60000) { s_needsRedraw = true; s_lastMinute = millis(); }

    // Touch
    TouchEvent evt = touchPoll();
    if (evt.swipe == SwipeDir::Left) {
        gotoScreen(s_screen + 1);
    } else if (evt.swipe == SwipeDir::Right) {
        gotoScreen(s_screen - 1);
    } else if (evt.tap == TapEvent::Tap) {
        int tx = evt.tapX, ty = evt.tapY;
        int botY = SCREEN_H - BOTBAR_H;
        // Corner arrow tap zones — bottom-left < and bottom-right >
        if (ty >= botY) {
            if (tx < 50)               gotoScreen(s_screen - 1);
            else if (tx > SCREEN_W - 50) gotoScreen(s_screen + 1);
        }
        if (s_screen == 3) {
            bool changed = screenSettingsTap(tft, tx, ty);
            if (changed) s_needsRedraw = true;
            if (screenSettingsRefreshTapped()) {
                showSplash("Refreshing...");
                locationFetch();
                fetchWeather();
            }
        }
    }

    if (s_needsRedraw) redraw();
    delay(20);
}
