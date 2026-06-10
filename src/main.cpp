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
#include "ui/screens/screen_fires.h"
#include "ui/screens/screen_usgs.h"
#include "ui/screens/screen_volcanoes.h"
#include "ui/screens/screen_news.h"
#include "ui/screens/screen_planner.h"
#include "ui/screens/screen_scanner.h"
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
static char          s_updateStr[24]  = "Never";

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
    tw = tft.textWidth("xXCYD-Weather-StationXx");
    tft.setCursor((SCREEN_W - tw) / 2, 152);
    tft.print("xXCYD-Weather-StationXx");

    tft.setTextColor(g_themeColor, COL_BG);
    tw = tft.textWidth("xXQuantum-SmokeXx");
    tft.setCursor((SCREEN_W - tw) / 2, 172);
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
static void fetchWeatherSync() {
    if (!s_wifiOk) return;
    if (!g_location.valid) { showSplash("Location failed.\nCheck serial monitor."); delay(3000); return; }
    ledSet(false, false, true);
    bool ok = weatherFetch(g_location.lat, g_location.lon);
    if (ok) {
        char t[10]; timeGetShort(t);
        snprintf(s_updateStr, sizeof(s_updateStr), "Updated %s", t);
        if (timeIsValid()) {
            time_t now = time(nullptr);
            s_lastWeatherHour = localtime(&now)->tm_hour;
        }
    }
    ledSet(false, false, false);
    s_needsRedraw = true;
}

// Screens: 0=Now, 1=Hourly, 2=5-Day, 3=Solar, 4=Fires, 5=USGS, 6=Volcanoes, 7=News, 8=Planner, 9=Scanner, 10=Settings
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
        case 4: screenFiresDraw(target, s_wifiOk);    break;
        case 5: screenUsgsDraw(target, s_wifiOk);     break;
        case 6: screenVolcanoesDraw(target, s_wifiOk); break;
        case 7: screenNewsDraw(target, s_wifiOk);     break;
        case 8: screenPlannerDraw(target, s_wifiOk);  break;
        case 9: screenScannerDraw(target, s_wifiOk);  break;
        case 10: screenSettingsDraw(target, s_wifiOk); break;
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

// ── First-boot touch calibration ─────────────────────────────────────────────
// Shows a target in the top-left corner. If the user taps it and the reported
// position lands in the bottom-right (180° opposite), touch needs flipping.
// Runs once; persisted via NVS key "touch_cal". Serial 'T' can toggle any time.
static void touchCalibrate() {
#if CYD_USB_VERSION == 2
    if (nvsGetInt("touch_cal", 0) != 0) return;

    // Backlight on in case brightnessInit hasn't run yet
    digitalWrite(TFT_BL, HIGH);

    tft.fillScreen(COL_BG);

    // Draw target box in top-left
    const int TX = 20, TY = CONTENT_Y, TW = 80, TH = 50;
    tft.fillRect(TX, TY, TW, TH, g_themeColor);
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_BG, g_themeColor);
    tft.setCursor(TX + 8, TY + TH / 2 - 8);
    tft.print("TAP");

    // Instructions at screen center — invariant under 180° rotation,
    // so the text is readable even if touch orientation is wrong.
    {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(COL_WHITE, COL_BG);
        const char *msg = "Tap the box above";
        int tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 - 20);
        tft.print(msg);

        tft.setTextFont(FONT_SM);
        tft.setTextColor(COL_DIM, COL_BG);
        msg = "(or wait 15s for default)";
        tw = tft.textWidth(msg);
        tft.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2);
        tft.print(msg);
    }

    // Poll for a single tap
    unsigned long start = millis();
    while (millis() - start < 15000) {
        TouchEvent evt = touchPoll();
        if (evt.tap == TapEvent::Tap) {
            int tx = evt.tapX, ty = evt.tapY;
            // If the user tapped the visible target (top-left) but the
            // touch reports bottom-right coords, the digitizer is 180° off.
            // Target: x=20..100, y=27..77  →  180°-rotated: x=220..300, y=163..213
            if (tx >= 220 && tx <= 300 && ty >= 163 && ty <= 213) {
                touchSetFlipped(!touchGetFlipped());
            }
            break;
        }
        delay(20);
    }

    nvsPutInt("touch_cal", 1);
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
    // CYD 2.8" two hardware revisions:
    //   ESP32-32E (1-USB): standard landscape → rotation 1
    //   2-USB:              landscape + Mirror Y, MADCTL = MY, no invert
#if CYD_USB_VERSION == 2
    tft.setRotation(1);           // landscape memory window
    tft.writecommand(TFT_MADCTL);
    tft.writedata(TFT_MAD_MY);    // mirror Y only
#else
    tft.setRotation(1);           // ESP32-32E: standard landscape
#endif

    tft.fillScreen(COL_BG);
    brightnessInit();

    // Touch on VSPI (separate from TFT_eSPI's HSPI bus)
    touchInit();

    // First-boot calibration — only on 2USB, only once
    touchCalibrate();

    showSplash("Starting up...");
    ledSet(false, true, false);

    connectWifi();
    s_wifiOk = (WiFi.status() == WL_CONNECTED);

    s_dataMutex = xSemaphoreCreateMutex();

    if (s_wifiOk) {
        showSplash("Getting location...");
        locationLoad();
        // Always fetch fresh — NVS cache may have stale/missing offset
        locationFetch();

        showSplash("Syncing time...");
        // Use UTC offset from ip-api immediately — correct time before weather loads
        timeSyncInit(g_location.valid ? g_location.utcOffset : 0);
        for (int i = 0; i < 30 && !timeIsValid(); i++) delay(100);   // 3 s max

        showSplash("Fetching weather...");
        fetchWeatherSync();

        screenshotInit(tft, redrawTo);
    }

    // Launch async fetch worker on Core 0
    xTaskCreatePinnedToCore(fetchWorker, "fetch", 16384, nullptr, 1, &s_fetchTask, 0);

    // Kick off background fetches — each message waits for that stage to complete
    if (s_wifiOk) {
        s_fetchCmd = FETCH_BOOT_BG;
        xTaskNotifyGive(s_fetchTask);

        showSplash("Fetching fires...");
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

    // Weather refresh — top of each new hour (async, Core 0)
    if (s_wifiOk && !workerBusy()) {
        bool shouldFetch = false;
        if (timeIsValid()) {
            time_t now = time(nullptr);
            int curHour = localtime(&now)->tm_hour;
            if (curHour != s_lastWeatherHour) shouldFetch = true;
        }
        if (shouldFetch && millis() - s_lastWeatherAttempt > 60000) {
            s_lastWeatherAttempt = millis();
            triggerFetch(false);
        }
    }

    // Hourly auto-refresh for all data screens (async, Core 0)
    // Each triggers once per hour, independent of screen navigation
    if (s_wifiOk && !workerBusy()) {
        if (timeIsValid()) {
            time_t now = time(nullptr);
            int curHour = localtime(&now)->tm_hour;

            if (!g_solarPending && curHour != s_lastSolarHour && millis() - s_lastSolarAttempt > 60000) {
                s_lastSolarAttempt = millis(); s_lastSolarHour = -1;
                g_solarPending = true; triggerSolarFetch();
            }
            if (!g_firesPending && curHour != s_lastFiresHour && millis() - s_lastFiresAttempt > 60000) {
                s_lastFiresAttempt = millis(); s_lastFiresHour = -1;
                g_firesPending = true; triggerFiresFetch();
            }
            if (!g_usgsPending && curHour != s_lastUsgsHour && millis() - s_lastUsgsAttempt > 60000) {
                s_lastUsgsAttempt = millis(); s_lastUsgsHour = -1;
                g_usgsPending = true; triggerUsgsFetch();
            }
            if (!g_newsPending && curHour != s_lastNewsHour && millis() - s_lastNewsAttempt > 60000) {
                s_lastNewsAttempt = millis(); s_lastNewsHour = -1;
                g_newsPending = true; triggerNewsFetch();
            }
            if (!g_volcanoesPending && curHour != s_lastVolcanoesHour && millis() - s_lastVolcanoesAttempt > 60000) {
                s_lastVolcanoesAttempt = millis(); s_lastVolcanoesHour = -1;
                g_volcanoesPending = true; triggerVolcanoesFetch();
            }
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
        s_firesDone = false;
        g_firesPending = false;
        xSemaphoreGive(s_dataMutex);
        if (timeIsValid()) { time_t now = time(nullptr); s_lastFiresHour = localtime(&now)->tm_hour; }
        if (s_screen == 4) s_needsRedraw = true;
    }

    // USGS fetch completion
    if (s_usgsDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_usgsDone = false;
        g_usgsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (timeIsValid()) { time_t now = time(nullptr); s_lastUsgsHour = localtime(&now)->tm_hour; }
        if (s_screen == 5) s_needsRedraw = true;
    }

    // Volcanoes fetch completion
    if (s_volcanoesDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_volcanoesDone = false;
        g_volcanoesPending = false;
        xSemaphoreGive(s_dataMutex);
        if (timeIsValid()) { time_t now = time(nullptr); s_lastVolcanoesHour = localtime(&now)->tm_hour; }
        if (s_screen == 6) s_needsRedraw = true;
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
        s_newsDone = false;
        g_newsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (timeIsValid()) { time_t now = time(nullptr); s_lastNewsHour = localtime(&now)->tm_hour; }
        if (s_screen == 7) s_needsRedraw = true;  // News
    }

    // Clock tick — only update the time text, not a full redraw
    if (millis() - s_lastMinute > 60000) {
        if (s_screen <= 2 || s_screen >= 7) {
            char timeStr[10]; timeGetShort(timeStr);
            static const char* labels[] = {"NOW","HOURLY","5-DAY","SOLAR","FIRES","USGS","VOLCANOES","NEWS","ALMANAC","SCANNER","SETTINGS"};
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

    // Scanner animation — light sweep-line update, no full redraw
    if (s_screen == 9 && !workerBusy()) {
        static unsigned long lastScannerFrame = 0;
        if (millis() - lastScannerFrame > 80) {
            screenScannerAnimate(tft);
            lastScannerFrame = millis();
        }
    }

    // Touch
    TouchEvent evt = touchPoll();
    if (evt.swipe == SwipeDir::Left) {
        gotoScreen(s_screen + 1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Right) {
        gotoScreen(s_screen - 1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 4) {
        screenFiresSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 4) {
        screenFiresSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 5) {
        screenUsgsSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 5) {
        screenUsgsSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 6) {
        screenVolcanoesSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 6) {
        screenVolcanoesSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Up && s_screen == 7) {
        screenNewsSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 7) {
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
        if (ty < TOPBAR_H && s_wifiOk && !workerBusy() && (s_screen <= 2 || s_screen == 8)) {
            showSplash("Refreshing...");
            triggerFetch(true);
        }
        if (s_screen == 3) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenSolarTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 4) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenFiresTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 5) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenUsgsTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 6) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenVolcanoesTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 7) {
            if (ty < TOPBAR_H && s_wifiOk) {
                screenNewsTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 8) {
            screenPlannerTap(tft, tx, ty, s_wifiOk);
            s_needsRedraw = true;
        } else if (s_screen == 10) {
            bool changed = screenSettingsTap(tft, tx, ty);
            if (changed) s_needsRedraw = true;
            if (screenSettingsRefreshTapped()) {
                showSplash("Refreshing...");
                triggerFetch(true);
            }
        }
    }

    screenshotLoop();
    if (screenshotNeedsRedraw()) s_needsRedraw = true;

    // Serial commands: '0'-'9' = switch to screen 0-9, 'A' = screen 10 (Settings), 'R' = ready check, 'S' = capture
    if (Serial.available()) {
        int cmd = Serial.read();
        if (cmd == 'R' || cmd == 'r') {
            Serial.println("READY");
        }
        if (cmd == 'T' || cmd == 't') {
            touchSetFlipped(!touchGetFlipped());
            nvsPutInt("touch_cal", 1);   // skip first-boot calibration from now on
            Serial.print("TOUCH:");
            Serial.println(touchGetFlipped() ? "FLIPPED" : "NORMAL");
            s_needsRedraw = true;
        }
        if (cmd >= '0' && cmd <= '9') {
            int n = cmd - '0';
            gotoScreen(n);
            redraw();
        } else if (cmd == 'A' || cmd == 'a') {
            gotoScreen(10);
            redraw();
        }
        if (cmd == 'S' || cmd == 's') {
            // 8-bit RGB332 capture — 16-bit sprites don't fit in free heap.
            // Font-7 digits on the NOW screen are patched with tiny 16-bit overlays.
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

    // Skip redraw while worker writes data globals on Core 0
    if (s_needsRedraw && !workerBusy()) redraw();
    delay(20);
}
