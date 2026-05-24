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
#include "ui/screens/screen_news.h"
#include "modules/screenshot.h"

static TFT_eSPI tft;

bool g_spriteCapture = false;


// ── App state ─────────────────────────────────────────────────────────────
static int           s_screen         = 0;
static bool          s_needsRedraw    = true;
static bool          s_wifiOk         = false;
static int           s_lastWeatherHour     = -1;   // hour (0-23) of last fetch; -1 = never
static unsigned long s_lastWeatherAttempt  = 0;    // cooldown timer to prevent tight retry loops
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
                g_solarPending  = true;
                g_newsPending   = true;

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

// Screens: 0=Now, 1=Hourly, 2=5-Day, 3=Solar, 4=Fires, 5=USGS, 6=News, 7=Settings
static void gotoScreen(int n) {
    s_screen         = constrain(n, 0, 7);
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
        case 6: screenNewsDraw(target, s_wifiOk);     break;
        case 7: screenSettingsDraw(target, s_wifiOk); break;
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
    xTaskCreatePinnedToCore(fetchWorker, "fetch", 8192, nullptr, 1, &s_fetchTask, 0);

    // Kick off background fetches — each message waits for that stage to complete
    if (s_wifiOk) {
        s_fetchCmd = FETCH_BOOT_BG;
        xTaskNotifyGive(s_fetchTask);

        showSplash("Fetching fires...");
        for (int i = 0; i < 60 && !s_firesDone; i++) delay(50);

        showSplash("Fetching USGS...");
        for (int i = 0; i < 60 && !s_usgsDone; i++) delay(50);

        showSplash("Fetching solar...");
        for (int i = 0; i < 60 && !s_solarDone; i++) delay(50);

        showSplash("Fetching news...");
        for (int i = 0; i < 60 && !s_newsDone; i++) delay(50);
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
        if (s_screen == 4) s_needsRedraw = true;
    }

    // USGS fetch completion
    if (s_usgsDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_usgsDone = false;
        g_usgsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (s_screen == 5) s_needsRedraw = true;
    }

    // Solar fetch completion
    if (s_solarDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_solarDone = false;
        g_solarPending = false;
        xSemaphoreGive(s_dataMutex);
        if (s_screen == 3) s_needsRedraw = true;
    }

    // News fetch completion
    if (s_newsDone) {
        xSemaphoreTake(s_dataMutex, portMAX_DELAY);
        s_newsDone = false;
        g_newsPending = false;
        xSemaphoreGive(s_dataMutex);
        if (s_screen == 6) s_needsRedraw = true;
    }

    // Clock tick — only update the time text, not a full redraw
    if (millis() - s_lastMinute > 60000) {
        if (s_screen <= 2 || s_screen >= 6) {
            char timeStr[10]; timeGetShort(timeStr);
            static const char* labels[] = {"NOW","HOURLY","5-DAY","SOLAR","FIRES","USGS","NEWS","SETTINGS"};
            drawTopbarTime(tft, timeStr, labels[s_screen]);
        }
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
    } else if (evt.swipe == SwipeDir::Up && s_screen == 6) {
        screenNewsSwipe(1);
        s_needsRedraw = true;
    } else if (evt.swipe == SwipeDir::Down && s_screen == 6) {
        screenNewsSwipe(-1);
        s_needsRedraw = true;
    } else if (evt.tap == TapEvent::Tap) {
        int tx = evt.tapX, ty = evt.tapY;
        int botY = SCREEN_H - BOTBAR_H;
        // Corner arrow tap zones — bottom-left < and bottom-right >
        if (ty >= botY) {
            if (tx < 50)               gotoScreen(s_screen - 1);
            else if (tx > SCREEN_W - 50) gotoScreen(s_screen + 1);
        }
        // Top bar tap → refresh (weather screens 0-2,6; data screens handle their own)
        if (ty < TOPBAR_H && s_wifiOk && !workerBusy() && (s_screen <= 2 || s_screen == 7)) {
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
                screenNewsTap(tft, tx, ty, s_wifiOk);
                s_needsRedraw = true;
            }
        } else if (s_screen == 7) {
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

    // Serial commands: '0'-'7' = switch screen, 'R' = ready check, 'S' = capture
    if (Serial.available()) {
        int cmd = Serial.read();
        if (cmd == 'R' || cmd == 'r') {
            Serial.println("READY");
        }
        if (cmd >= '0' && cmd <= '7') {
            gotoScreen(cmd - '0');
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
