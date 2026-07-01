#include "screen_solar.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

struct SolarState {
    bool valid = false;
    bool fetchedOnce = false;
    unsigned long fetchedMs = 0;
    float kp = 0;
    float kpHist[8] = {0};
    int histCount = 0;
    float windKms = 0;
    float density = 0;
    float bz = 0;
    float bt = 0;
    float solarTemp = 0;
    float xrayFlux = 0;
    float cmeSpeedKms = 0;
    char xrayClass[8] = "N/A";
    char flareClass[8] = "NONE";
    char flareTime[14] = "---";
    char cmeTime[14] = "NONE";
    char sync[10] = "--:--";
};

static SolarState s_solar;
static bool s_forceRefresh = false;
bool g_solarPending = false;

static const char *kpCondition(float kp) {
    if (kp < 3) return "QUIET";
    if (kp < 4) return "UNSETTLED";
    if (kp < 5) return "ACTIVE";
    if (kp < 6) return "G1 MINOR";
    if (kp < 7) return "G2 MODERATE";
    if (kp < 8) return "G3 STRONG";
    if (kp < 9) return "G4 SEVERE";
    return "G5 EXTREME";
}

static uint16_t kpColor(float kp) {
    if (kp >= 5) return COL_RED;
    if (kp >= 3) return COL_AMBER;
    return g_themeColor;
}

static const char *auroraLabel(float kp) {
    if (kp < 5) return "NONE";
    if (kp < 7) return "HIGH LAT";
    if (kp < 8) return "POSSIBLE";
    return "LIKELY";
}

static bool readKp(JsonVariant row, float &kp) {
    JsonVariant v;
    if (row.is<JsonArray>()) v = row[1];
    else {
        v = row["Kp"];
        if (v.isNull()) v = row["kp"];
    }
    if (v.isNull()) return false;
    if (v.is<float>() || v.is<int>()) {
        kp = v.as<float>();
        return true;
    }
    const char *s = v.as<const char *>();
    if (!s || !isdigit((unsigned char)s[0])) return false;
    kp = atof(s);
    return true;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_solar.sync, sizeof(s_solar.sync), "%s", t);
}

static void fluxToClass(float flux, char *out, size_t len) {
    if (flux <= 0) { snprintf(out, len, "N/A"); return; }
    const char letters[] = "ABCMX";
    const float bounds[] = { 1e-8f, 1e-7f, 1e-6f, 1e-5f, 1e-4f };
    int idx = 0;
    for (int i = 4; i >= 0; --i) {
        if (flux >= bounds[i]) { idx = i; break; }
    }
    snprintf(out, len, "%c%.1f", letters[idx], flux / bounds[idx]);
}

static void getUtcDate(int dayOffset, char *out, size_t len) {
    time_t now = time(nullptr);
    if (now < 1000000) {
        snprintf(out, len, "2026-05-21");
        return;
    }
    now += (time_t)dayOffset * 86400;
    struct tm *ti = gmtime(&now);
    snprintf(out, len, "%04d-%02d-%02d", ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
}

static bool fetchKp() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray arr = doc.as<JsonArray>();
    float latest[8];
    int count = 0;
    for (int i = (int)arr.size() - 1; i >= 0 && count < 8; --i) {
        float kp = 0;
        if (readKp(arr[i], kp)) latest[count++] = kp;
    }
    if (count == 0) return false;
    s_solar.kp = latest[0];
    s_solar.histCount = count;
    for (int i = 0; i < count; ++i) s_solar.kpHist[i] = latest[count - 1 - i];
    for (int i = count; i < 8; ++i) s_solar.kpHist[i] = s_solar.kp;
    return true;
}

static void fetchPlasma() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/solar-wind/plasma-2-hour.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc.as<JsonArray>();
            int last = arr.size() - 1;
            if (last > 0) {
                if (!arr[last][1].isNull()) s_solar.density = arr[last][1].as<float>();
                if (!arr[last][2].isNull()) s_solar.windKms = arr[last][2].as<float>();
                if (!arr[last][3].isNull()) s_solar.solarTemp = arr[last][3].as<float>();
            }
        }
    }
    http.end();
}

static void fetchMag() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/products/solar-wind/mag-2-hour.json");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc.as<JsonArray>();
            int last = arr.size() - 1;
            if (last > 0) {
                if (!arr[last][3].isNull()) s_solar.bz = arr[last][3].as<float>();
                if (!arr[last][4].isNull()) s_solar.bt = arr[last][4].as<float>();
            }
        }
    }
    http.end();
}

static void fetchXray() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json");
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return; }

    static const int TAIL_MAX = 500;
    char tail[TAIL_MAX + 1];
    int tailLen = 0;
    uint8_t chunk[128];
    WiFiClient *stream = http.getStreamPtr();
    uint32_t deadline = millis() + 12000;

    while (millis() < deadline) {
        int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected()) break;
            delay(5);
            continue;
        }
        int n = stream->readBytes(chunk, min(avail, (int)sizeof(chunk)));
        if (n <= 0) break;
        if (tailLen + n <= TAIL_MAX) {
            memcpy(tail + tailLen, chunk, n);
            tailLen += n;
        } else {
            int keep = TAIL_MAX - n;
            if (keep < 0) keep = 0;
            if (keep > 0) memmove(tail, tail + tailLen - keep, keep);
            int copyFrom = (n > TAIL_MAX) ? n - TAIL_MAX : 0;
            int copyLen = min(n, TAIL_MAX);
            memcpy(tail + keep, chunk + copyFrom, copyLen);
            tailLen = keep + copyLen;
        }
    }
    http.end();
    tail[tailLen] = '\0';

    char *pos = nullptr;
    for (char *p = tail; (p = strstr(p, "0.1-0.8nm")) != nullptr; ++p) pos = p;
    if (!pos) return;
    for (char *fp = pos; fp > tail; --fp) {
        if (strncmp(fp, "\"flux\"", 6) == 0) {
            char *colon = strchr(fp, ':');
            if (!colon) continue;
            s_solar.xrayFlux = atof(colon + 1);
            fluxToClass(s_solar.xrayFlux, s_solar.xrayClass, sizeof(s_solar.xrayClass));
            return;
        }
    }
}

static void fetchFlare() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json");
    http.setTimeout(8000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return;
    JsonObject flare = arr[0];
    const char *cls = flare["max_class"] | flare["current_class"] | "NONE";
    const char *peak = flare["max_time"] | flare["time_tag"] | "";
    snprintf(s_solar.flareClass, sizeof(s_solar.flareClass), "%s", cls);
    if (peak && strlen(peak) >= 16) snprintf(s_solar.flareTime, sizeof(s_solar.flareTime), "%.5s %.5s", peak + 5, peak + 11);
}

static void fetchCme() {
    char startDate[12], endDate[12], url[220];
    getUtcDate(-4, startDate, sizeof(startDate));
    getUtcDate(0, endDate, sizeof(endDate));
    snprintf(url, sizeof(url), "https://api.nasa.gov/DONKI/CME?startDate=%s&endDate=%s&api_key=DEMO_KEY", startDate, endDate);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return;

    JsonObject cme = arr[arr.size() - 1];
    const char *start = cme["startTime"] | "";
    s_solar.cmeSpeedKms = 0;
    for (JsonVariant a : cme["cmeAnalyses"].as<JsonArray>()) {
        if (!a["speed"].isNull()) {
            s_solar.cmeSpeedKms = a["speed"].as<float>();
            break;
        }
    }
    if (start && strlen(start) >= 16) snprintf(s_solar.cmeTime, sizeof(s_solar.cmeTime), "%.5s %.5s", start + 5, start + 11);
}

void solarFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) {
        return;
    }

    // Save old state in case Kp fetch fails
    SolarState old = s_solar;

    // Reset fields that sub-fetchers will populate
    snprintf(s_solar.xrayClass, sizeof(s_solar.xrayClass), "N/A");
    snprintf(s_solar.flareClass, sizeof(s_solar.flareClass), "NONE");
    snprintf(s_solar.flareTime, sizeof(s_solar.flareTime), "---");
    snprintf(s_solar.cmeTime, sizeof(s_solar.cmeTime), "NONE");
    s_solar.cmeSpeedKms = 0;
    s_solar.bt = 0;
    s_solar.solarTemp = 0;

    bool kpOk = fetchKp();
    fetchPlasma();
    fetchMag();
    fetchXray();
    fetchFlare();
    fetchCme();

    if (!kpOk) {
        // Restore old data on Kp failure (Kp is the critical piece)
        s_solar = old;
        return;
    }
    s_solar.valid = true;
    s_solar.fetchedMs = millis();
    s_solar.fetchedOnce = true;
    stampSync();
}

static void drawSolarContent(TFT_eSPI &tft) {
    uint16_t kc = kpColor(s_solar.kp);
    char buf[32];

    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, CONTENT_Y + 6);
    tft.print("Kp INDEX");

    snprintf(buf, sizeof(buf), "%.1f", s_solar.kp);
    tft.setTextFont(FONT_NUM);
    tft.setTextColor(kc, COL_BG);
    tft.setCursor(8, CONTENT_Y + 18);
    tft.print(buf);

    tft.setTextFont(FONT_MD);
    tft.setTextColor(kc, COL_BG);
    tft.setCursor(8, CONTENT_Y + 74);
    tft.print(kpCondition(s_solar.kp));

    tft.drawFastVLine(112, CONTENT_Y + 4, 82, g_themeColor);

    struct Stat { const char *label; char value[18]; uint16_t color; } stats[6];
    snprintf(stats[0].value, sizeof(stats[0].value), "%.0f km/s", s_solar.windKms);
    stats[0].label = "WIND"; stats[0].color = COL_WHITE;
    snprintf(stats[1].value, sizeof(stats[1].value), "%.1f nT", s_solar.bz);
    stats[1].label = "Bz"; stats[1].color = COL_WHITE;
    snprintf(stats[2].value, sizeof(stats[2].value), "%.1f p/cc", s_solar.density);
    stats[2].label = "DENS"; stats[2].color = COL_WHITE;
    snprintf(stats[3].value, sizeof(stats[3].value), "%s", s_solar.xrayClass);
    stats[3].label = "XRAY"; stats[3].color = COL_AMBER;
    snprintf(stats[4].value, sizeof(stats[4].value), "%s", s_solar.flareClass);
    stats[4].label = "FLR"; stats[4].color = COL_AMBER;
    if (s_solar.cmeSpeedKms > 0) snprintf(stats[5].value, sizeof(stats[5].value), "%.0f", s_solar.cmeSpeedKms);
    else snprintf(stats[5].value, sizeof(stats[5].value), "%s", s_solar.cmeTime);
    stats[5].label = "CME kms"; stats[5].color = (s_solar.cmeSpeedKms > 0 || strcmp(s_solar.cmeTime, "NONE") != 0) ? COL_RED : COL_WHITE;

    for (int i = 0; i < 6; ++i) {
        int col = i / 3;
        int row = i % 3;
        int x = col == 0 ? 124 : 196;
        int y = CONTENT_Y + 8 + row * 25;
        tft.setTextFont(FONT_SM);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(x, y);
        tft.print(stats[i].label);
        tft.setTextFont(FONT_MD);
        tft.setTextColor(stats[i].color, COL_BG);
        tft.setCursor(x, y + 9);
        tft.print(stats[i].value);
    }

    // 3rd column — BT, TEMP, AURORA (labels FONT_SM, values FONT_MD)
    int ax = 252, ay = CONTENT_Y + 8;

    // BT — row 0
    char btBuf[12];
    snprintf(btBuf, sizeof(btBuf), "%.1f nT", s_solar.bt);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay);
    tft.print("BT");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(ax, ay + 9);
    tft.print(btBuf);

    // TEMP — row 1
    char tempBuf[12];
    if (s_solar.solarTemp >= 1000000)
        snprintf(tempBuf, sizeof(tempBuf), "%.1fM K", s_solar.solarTemp / 1000000.0f);
    else if (s_solar.solarTemp >= 1000)
        snprintf(tempBuf, sizeof(tempBuf), "%.0fK K", s_solar.solarTemp / 1000.0f);
    else if (s_solar.solarTemp > 0)
        snprintf(tempBuf, sizeof(tempBuf), "%.0f K", s_solar.solarTemp);
    else
        snprintf(tempBuf, sizeof(tempBuf), "N/A");
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay + 25);
    tft.print("TEMP");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(ax, ay + 25 + 9);
    tft.print(tempBuf);

    // AURORA — row 2 (bottom)
    const char *al = auroraLabel(s_solar.kp);
    uint16_t ac = COL_WHITE;
    if (s_solar.kp < 5)       ac = COL_WHITE;
    else if (s_solar.kp < 7)  ac = COL_AMBER;
    else if (s_solar.kp < 8)  ac = COL_WHITE;
    else                      ac = COL_RED;
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(ax, ay + 50);
    tft.print("AURORA");
    tft.setTextFont(FONT_MD);
    tft.setTextColor(ac, COL_BG);
    tft.setCursor(ax, ay + 50 + 9);
    tft.print(al);

    int chartY = CONTENT_Y + 104;
    tft.drawFastHLine(0, chartY - 8, SCREEN_W, g_themeColor);
    tft.setTextFont(FONT_SM);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.setCursor(8, chartY - 5);
    tft.print("24H Kp HISTORY");

    const int barW = 30;
    const int gap = 8;
    const int maxH = 56;
    const int startX = (SCREEN_W - (8 * barW + 7 * gap)) / 2;
    const int baseY = chartY + 68;
    for (int i = 0; i < 8; ++i) {
        float kp = s_solar.kpHist[i];
        int h = (int)(kp / 9.0f * maxH);
        if (h < 2) h = 2;
        int x = startX + i * (barW + gap);
        uint16_t c = kpColor(kp);
        tft.drawRect(x, baseY - maxH, barW, maxH, COL_DIM);
        tft.fillRect(x + 1, baseY - h, barW - 2, h, c);
        snprintf(buf, sizeof(buf), "%.0f", kp);
        tft.setTextFont(FONT_SM);
        tft.setTextColor(c, COL_BG);
        int tw = tft.textWidth(buf);
        tft.setCursor(x + (barW - tw) / 2, baseY + 4);
        tft.print(buf);
    }
}

void screenSolarDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "SOLAR", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 3, 12);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    // Only fetch on first-ever visit or explicit top-bar tap refresh.
    // Time-based staleness is handled by the hourly auto-refresh in the main loop.
    bool doFetch = (!s_solar.fetchedOnce || s_forceRefresh) && wifiOk && !g_solarPending;
    s_forceRefresh = false;

    if (doFetch) {
        g_solarPending = true;
        triggerSolarFetch();
    }

    if (s_solar.valid) {
        drawSolarContent(tft);
    } else if (g_solarPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching solar...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(92, 100);
        tft.print(wifiOk ? "No solar data" : "Solar offline");
        tft.setTextFont(FONT_SM);
        tft.setCursor(58, 124);
        tft.print("Connect WiFi and revisit this screen");
    }
}

void screenSolarTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_solar.fetchedMs = 0;
    }
}
