#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

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
#include "ui/screens/screen_fires.h"
#include "ui/screens/screen_usgs.h"
#include "modules/screenshot.h"

static TFT_eSPI tft;

// Set true during 8-bit sprite capture so screens can swap font 7 -> FONT_LG
bool g_spriteCapture = false;

// ── App state ─────────────────────────────────────────────────────────────
static int           s_screen         = 0;
static bool          s_needsRedraw    = true;
static bool          s_wifiOk         = false;
static unsigned long s_lastWeather    = 0;
static int           s_lastWeatherHour = -1;   // hour (0-23) of last fetch; -1 = never
static unsigned long s_lastMinute     = 0;
static unsigned long s_lastAutoRotate  = 0;
static char          s_updateStr[24]  = "Never";

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
    char ssid[64], pass[64];
    wifiGetSSID(ssid, sizeof(ssid));
    wifiGetPass(pass, sizeof(pass));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
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
    if (timeIsValid()) {
        time_t now = time(nullptr);
        s_lastWeatherHour = localtime(&now)->tm_hour;
    }
    s_needsRedraw = true;
}

// Screens: 0=Now, 1=Hourly, 2=5-Day, 3=Solar, 4=Fires, 5=USGS, 6=Settings
static void gotoScreen(int n) {
    s_screen         = constrain(n, 0, 6);
    s_lastAutoRotate = millis();
    s_needsRedraw    = true;
}

static void redrawTo(TFT_eSPI &target) {
    switch (s_screen) {
        case 0: screenCurrentDraw(target, s_wifiOk);  break;
        case 1: screenHourlyDraw(target, s_wifiOk);   break;
        case 2: screenForecastDraw(target, s_wifiOk); break;
        case 3: screenSolarDraw(target, s_wifiOk);    break;
        case 4: screenFiresDraw(target, s_wifiOk);    break;
        case 5: screenUsgsDraw(target, s_wifiOk);     break;
        case 6: screenSettingsDraw(target, s_wifiOk); break;
    }
}

static uint8_t rgb565To332(uint16_t c) {
    uint8_t r3 = (c >> 13) & 0x07;
    uint8_t g3 = (c >> 8)  & 0x07;
    uint8_t b2 = (c >> 3)  & 0x03;
    return (r3 << 5) | (g3 << 2) | b2;
}

static void fillCaptureRect(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    if (!fb || w <= 0 || h <= 0) return;
    int x0 = constrain(x, 0, SCREEN_W);
    int y0 = constrain(y, 0, SCREEN_H);
    int x1 = constrain(x + w, 0, SCREEN_W);
    int y1 = constrain(y + h, 0, SCREEN_H);
    for (int yy = y0; yy < y1; yy++) {
        memset(fb + yy * SCREEN_W + x0, color, x1 - x0);
    }
}

static void overlayFont7CaptureText(uint8_t *fb, const char *text, int x, int y) {
    if (!fb || !text || !text[0]) return;

    tft.setTextFont(7);
    int w = tft.textWidth(text);
    int h = tft.fontHeight(7);
    if (w <= 0 || h <= 0) return;

    fillCaptureRect(fb, x, y, w, h, rgb565To332(COL_BG));

    TFT_eSprite patch(&tft);
    patch.setColorDepth(16);
    uint16_t *pfb = (uint16_t*)patch.createSprite(w, h);
    if (!pfb) return;

    patch.fillSprite(COL_BG);
    patch.setTextFont(7);
    patch.setTextColor(COL_WHITE, COL_BG);
    patch.setCursor(0, 0);
    patch.print(text);

    for (int py = 0; py < h; py++) {
        int dy = y + py;
        if (dy < 0 || dy >= SCREEN_H) continue;
        for (int px = 0; px < w; px++) {
            int dx = x + px;
            if (dx < 0 || dx >= SCREEN_W) continue;
            uint16_t c = patch.readPixel(px, py);
            if (c != COL_BG) fb[dy * SCREEN_W + dx] = rgb565To332(c);
        }
    }

    patch.deleteSprite();
}

static void patchNowCaptureDigits(uint8_t *fb) {
    if (!fb || !g_current.valid) return;

    const int ICY    = CONTENT_Y + 28;
    const int ICR    = 24;
    const int DX     = 118;
    const int LEFT_W = DX - 8;
    const int condY  = ICY + ICR + 5;
    const int hiValY = condY + 16 + 4;
    const int loValY = hiValY + 59;

    char buf[8];
    tft.setTextFont(7);

    snprintf(buf, sizeof(buf), "%d", (int)roundf(g_current.today_max));
    overlayFont7CaptureText(fb, buf, (LEFT_W - tft.textWidth(buf)) / 2, hiValY);

    snprintf(buf, sizeof(buf), "%d", (int)roundf(g_current.today_min));
    overlayFont7CaptureText(fb, buf, (LEFT_W - tft.textWidth(buf)) / 2, loValY);

    snprintf(buf, sizeof(buf), "%d", (int)roundf(g_current.temp));
    overlayFont7CaptureText(fb, buf, DX, CONTENT_Y + 12);
}
static void redraw() {
    redrawTo(tft);
    s_needsRedraw = false;
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

        screenshotInit(tft, redrawTo);
    }

    ledSet(false, false, false);
    s_needsRedraw = true;
    Serial.println("READY");
}

// ─────────────────────────────────────────────────────────────────────────
void loop() {
    // Brightness update (auto=LDR, fixed=no-op)
    static unsigned long lastLdr = 0;
    if (millis() - lastLdr > 5000) {
        brightnessAutoUpdate();
        lastLdr = millis();
    }

    // Weather refresh — top of each new hour; millis fallback until time is synced
    if (s_wifiOk) {
        if (timeIsValid()) {
            time_t now = time(nullptr);
            int curHour = localtime(&now)->tm_hour;
            if (curHour != s_lastWeatherHour) fetchWeather();
        } else if (millis() - s_lastWeather > WEATHER_REFRESH_MS) {
            fetchWeather();
        }
    }

    // Clock tick. The live data screens keep their network cache and avoid
    // full minute redraws to prevent visible flicker before rotation.
    if (millis() - s_lastMinute > 60000) {
        if (s_screen <= 2 || s_screen == 6) s_needsRedraw = true;
        s_lastMinute = millis();
    }

    // Auto-rotate screens 0→1→2→0
    if (screenSettingsGetAutoRotate() &&
        millis() - s_lastAutoRotate > screenSettingsGetAutoRotateMs()) {
        gotoScreen(s_screen < 5 ? s_screen + 1 : 0);
    }

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
        if (s_screen == 6) {
            bool changed = screenSettingsTap(tft, tx, ty);
            if (changed) s_needsRedraw = true;
            if (screenSettingsRefreshTapped()) {
                showSplash("Refreshing...");
                locationFetch();
                fetchWeather();
            }
        }
    }

    screenshotLoop();
    if (screenshotNeedsRedraw()) s_needsRedraw = true;

    // Serial commands: '0'-'6' = switch screen, 'S' = capture current screen
    if (Serial.available()) {
        int cmd = Serial.read();
        if (cmd == 'R' || cmd == 'r') {
            Serial.println("READY");
        }
        if (cmd >= '0' && cmd <= '6') {
            gotoScreen(cmd - '0');
            redraw();
        }
        if (cmd == 'S' || cmd == 's') {
            // Full 16-bit captures exceed heap on this CYD. Capture the screen
            // in 8-bit RGB332, then patch the NOW font-7 digits with small
            // 16-bit overlays so the preferred digit font is preserved.
            TFT_eSprite spr(&tft);
            spr.setColorDepth(8);
            uint8_t *fb = (uint8_t*)spr.createSprite(SCREEN_W, SCREEN_H);
            if (!fb) {
                Serial.print("OOM:");
                Serial.print(ESP.getFreeHeap());
                Serial.print(",max:");
                Serial.println(ESP.getMaxAllocHeap());
            } else {
                bool oldCapture = g_spriteCapture;
                g_spriteCapture = false;
                redrawTo(spr);
                g_spriteCapture = oldCapture;

                if (s_screen == 0) patchNowCaptureDigits(fb);

                Serial.print("RGB332:");
                Serial.write(fb, SCREEN_W * SCREEN_H);
                Serial.flush();
                spr.deleteSprite();
            }
        }
    }

    if (s_needsRedraw) redraw();
    delay(20);
}
