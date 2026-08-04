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

#define SOLAR_CACHE_MS (15UL * 60UL * 1000UL)

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

static bool stale() {
    if (!s_solar.fetchedOnce) return true;
    if (s_forceRefresh) return true;
    return millis() - s_solar.fetchedMs > SOLAR_CACHE_MS;
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

static bool fetchKp() {
    static WiFiClientSecure client;
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

static bool fetchPlasma() {
    // NOAA SWPC deprecated /products/solar-wind/plasma-*.json Apr 2026.
    // Replacement /json/rtsw/rtsw_wind_1m.json returns ~2.7 MB (multi-day,
    // multi-satellite), far too large for ESP32 JsonDocument.  Stream-read
    // only the first ~2 KB — newest data is prepended — and parse the first
    // JSON object.  Field mapping: density→proton_density, speed→proton_speed,
    // temperature→proton_temperature.
    static WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/rtsw/rtsw_wind_1m.json");
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return false; }

    static const int BUF_SZ = 2048;
    char buf[BUF_SZ + 1];
    int bufLen = 0;
    WiFiClient *stream = http.getStreamPtr();
    uint32_t deadline = millis() + 10000;

    while (bufLen < BUF_SZ && millis() < deadline) {
        int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected()) break;
            delay(5);
            continue;
        }
        int n = stream->readBytes((uint8_t*)(buf + bufLen),
                                  avail < (BUF_SZ - bufLen) ? avail : (BUF_SZ - bufLen));
        if (n <= 0) break;
        bufLen += n;
    }
    http.end();
    buf[bufLen] = '\0';

    // Find first JSON object: first '{' to matching '}' (no nested braces in these records)
    char *start = strchr(buf, '{');
    if (!start) return false;
    char *end = strchr(start, '}');
    if (!end) return false;
    *(end + 1) = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, start)) return false;
    // Only write fields that are present — old values survive on failure
    if (!doc["proton_density"].isNull())     s_solar.density  = doc["proton_density"].as<float>();
    if (!doc["proton_speed"].isNull())       s_solar.windKms  = doc["proton_speed"].as<float>();
    if (!doc["proton_temperature"].isNull()) s_solar.solarTemp = doc["proton_temperature"].as<float>();
    return true;
}

static bool fetchMag() {
    // NOAA SWPC deprecated /products/solar-wind/mag-*.json Apr 2026.
    // Replacement /json/rtsw/rtsw_mag_1m.json returns ~1.5 MB (multi-day,
    // multi-satellite), far too large for ESP32 JsonDocument.  Stream-read
    // only the first ~2 KB — newest data is prepended — and parse the first
    // JSON object.  Field names bz_gsm and bt are unchanged.
    static WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/rtsw/rtsw_mag_1m.json");
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return false; }

    static const int BUF_SZ = 2048;
    char buf[BUF_SZ + 1];
    int bufLen = 0;
    WiFiClient *stream = http.getStreamPtr();
    uint32_t deadline = millis() + 10000;

    while (bufLen < BUF_SZ && millis() < deadline) {
        int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected()) break;
            delay(5);
            continue;
        }
        int n = stream->readBytes((uint8_t*)(buf + bufLen),
                                  avail < (BUF_SZ - bufLen) ? avail : (BUF_SZ - bufLen));
        if (n <= 0) break;
        bufLen += n;
    }
    http.end();
    buf[bufLen] = '\0';

    // Find first JSON object: first '{' to matching '}' (no nested braces in these records)
    char *start = strchr(buf, '{');
    if (!start) return false;
    char *end = strchr(start, '}');
    if (!end) return false;
    *(end + 1) = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, start)) return false;
    // Only write fields that are present — old values survive on failure
    if (!doc["bz_gsm"].isNull()) s_solar.bz = doc["bz_gsm"].as<float>();
    if (!doc["bt"].isNull())     s_solar.bt = doc["bt"].as<float>();
    return true;
}

static bool fetchXray() {
    static WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json");
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return false; }

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
    if (!pos) return false;
    for (char *fp = pos; fp > tail; --fp) {
        if (strncmp(fp, "\"flux\"", 6) == 0) {
            char *colon = strchr(fp, ':');
            if (!colon) continue;
            s_solar.xrayFlux = atof(colon + 1);
            fluxToClass(s_solar.xrayFlux, s_solar.xrayClass, sizeof(s_solar.xrayClass));
            return true;
        }
    }
    return false;
}

static bool fetchFlare() {
    static WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json");
    http.setTimeout(8000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    if (http.GET() != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return false;
    JsonObject flare = arr[0];
    const char *cls = flare["max_class"] | flare["current_class"] | "NONE";
    const char *peak = flare["max_time"] | flare["time_tag"] | "";
    snprintf(s_solar.flareClass, sizeof(s_solar.flareClass), "%s", cls);
    if (peak && strlen(peak) >= 16)
        snprintf(s_solar.flareTime, sizeof(s_solar.flareTime), "%.5s %.5s", peak + 5, peak + 11);
    return true;
}

static bool fetchCme() {
    // Quantum-Meteor Worker proxies NASA DONKI (55 KB → ~100 bytes).
    // The worker extracts only the latest CME's startTime + speed, so the
    // ESP32 never touches the full payload.  Worker caches DONKI for 1 h
    // so we stay well under NASA DEMO_KEY's 50 req/day limit.
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/cme");
    http.setTimeout(10000);
    if (http.GET() != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    const char *start = doc["startTime"] | "";
    float speed = doc["speed"].as<float>();

    // Only update on successful parse — old values survive fetch failures
    if (start && strlen(start) >= 16) {
        s_solar.cmeSpeedKms = (speed > 0) ? speed : 0;
        snprintf(s_solar.cmeTime, sizeof(s_solar.cmeTime), "%.5s %.5s", start + 5, start + 11);
    } else if (speed > 0) {
        // Speed without a parseable time — still record it
        s_solar.cmeSpeedKms = speed;
        snprintf(s_solar.cmeTime, sizeof(s_solar.cmeTime), "NONE");
    } else {
        // No CME data from worker (legitimate empty state)
        s_solar.cmeSpeedKms = 0;
        snprintf(s_solar.cmeTime, sizeof(s_solar.cmeTime), "NONE");
    }
    return true;
}

void solarFetch(bool wifiOk) {
    if (!wifiOk || !WiFi.isConnected()) {
        return;
    }

    // Save old state in case Kp fetch fails
    SolarState old = s_solar;

    // NOTE: no longer pre-reset sub-fields to defaults.  Each fetch*()
    // returns bool and only writes to s_solar on success — old values
    // survive individual fetch failures.  The old Kp-only save/restore
    // remains as a last resort (Kp is the critical piece that anchors
    // the whole screen).

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
    drawBottombar(tft, dateStr, 3, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_solar.fetchedOnce || s_forceRefresh || stale()) && wifiOk && !g_solarPending;
    s_forceRefresh = false;

    if (doFetch) {
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
