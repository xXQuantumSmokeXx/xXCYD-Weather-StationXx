#include "weather.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <cstring>
#include <cstdio>

CurrentWeather  g_current;
HourlyWeather   g_hourly[HOURLY_COUNT];
DailyWeather    g_daily[DAILY_COUNT];
int             g_utcOffsetSec = 0;
WeatherAlert    g_weatherAlert;
bool            g_alertsChecked = false;
unsigned long   g_weatherUpdatedEpoch = 0;
int             g_weatherError = 0;

const char* wmoDescription(int c) {
    if (c == 0)   return "Clear";
    if (c == 1)   return "Mostly Clr";
    if (c == 2)   return "Pt Cloudy";
    if (c == 3)   return "Overcast";
    if (c <= 48)  return "Foggy";
    if (c <= 55)  return "Drizzle";
    if (c <= 65)  return "Rain";
    if (c <= 77)  return "Snow";
    if (c <= 82)  return "Showers";
    if (c <= 86)  return "Snow Shrs";
    if (c == 95)  return "T-Storm";
    if (c <= 99)  return "Hvy Storm";
    return "Unknown";
}

WxIcon wmoIcon(int c) {
    if (c == 0)            return WxIcon::Sun;
    if (c <= 2)            return WxIcon::PartlyCloudy;
    if (c == 3)            return WxIcon::Cloudy;
    if (c <= 48)           return WxIcon::Fog;
    if (c <= 55)           return WxIcon::Drizzle;
    if (c <= 65)           return WxIcon::Rain;
    if (c <= 77)           return WxIcon::Snow;
    if (c <= 82)           return WxIcon::Showers;
    if (c <= 86)           return WxIcon::Snow;
    return WxIcon::Storm;
}

const char* windCardinal(int deg) {
    static const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
    return dirs[((deg + 22) / 45) % 8];
}

// Parse YYYY-MM-DDTHH:00 → hour (0-23)
static int parseHour(const char *s) {
    // format: "2024-01-15T14:00"
    if (!s || strlen(s) < 16) return 0;
    return (s[11] - '0') * 10 + (s[12] - '0');
}

// Parse "YYYY-MM-DDTHH:MM" → "H:MMa/p" (12-hour)
static void parseSunTime(const char *iso, char *buf, int bufLen) {
    if (!iso || strlen(iso) < 16) { snprintf(buf, bufLen, "--:--"); return; }
    int h = (iso[11]-'0')*10 + (iso[12]-'0');
    int m = (iso[14]-'0')*10 + (iso[15]-'0');
    const char *ap = (h < 12) ? "a" : "p";
    int h12 = (h == 0 || h == 12) ? 12 : h % 12;
    snprintf(buf, bufLen, "%d:%02d%s", h12, m, ap);
}

// Parse day-of-week from YYYY-MM-DD string
static void parseDayName(const char *s, char *buf3) {
    static const char *days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    struct tm tm = {};
    // s = "2024-01-15"
    if (!s || strlen(s) < 10) { strncpy(buf3, "???", 3); return; }
    sscanf(s, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    mktime(&tm);
    strncpy(buf3, days[tm.tm_wday], 3);
    buf3[3] = '\0';
}


static void fetchNwsAlert(float lat, float lon) {
    g_weatherAlert = WeatherAlert{};
    g_alertsChecked = false;

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.weather.gov/alerts/active?point=%.4f,%.4f", lat, lon);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) return;
    http.setTimeout(12000);
    http.addHeader("User-Agent", "xXCYD-Weather-StationXx github.com/xXQuantumSmokeXx");
    http.addHeader("Accept", "application/geo+json");
    http.addHeader("Accept-Encoding", "identity");
    int code = http.GET();
    if (code != 200) { http.end(); return; }

    JsonDocument filter;
    filter["features"][0]["properties"]["event"] = true;
    filter["features"][0]["properties"]["severity"] = true;
    JsonDocument alertDoc;
    DeserializationError err = deserializeJson(
        alertDoc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) return;

    g_alertsChecked = true;
    JsonObjectConst props = alertDoc["features"][0]["properties"];
    const char *event = props["event"] | "";
    const char *severity = props["severity"] | "Unknown";
    if (event[0]) {
        g_weatherAlert.active = true;
        snprintf(g_weatherAlert.event, sizeof(g_weatherAlert.event), "%s", event);
        snprintf(g_weatherAlert.severity, sizeof(g_weatherAlert.severity), "%s", severity);
    }
}
bool weatherFetch(float lat, float lon) {
    char url[640];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature"
        ",weather_code,wind_speed_10m,wind_direction_10m"
        ",surface_pressure,visibility,uv_index"
        "&hourly=temperature_2m,weather_code,precipitation_probability"
        ",wind_speed_10m,relative_humidity_2m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        ",precipitation_probability_max,wind_speed_10m_max,relative_humidity_2m_max"
        ",sunrise,sunset"
        "&temperature_unit=fahrenheit"
        "&wind_speed_unit=mph"
        "&timezone=auto"
        "&forecast_days=5",
        lat, lon);

    WiFiClient client;
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("Accept-Encoding", "identity");  // prevent gzip — ArduinoJson can't decompress
    int code = http.GET();
    if (code != 200) {
        g_weatherError = code;
        http.end();
        return false;
    }
    g_weatherError = 0;

    String body = http.getString();
    http.end();

    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        g_weatherError = -(int)err.code();  // negative = JSON error
        return false;
    }

    // UTC offset (used by time sync)
    g_utcOffsetSec = doc["utc_offset_seconds"] | 0;

    // ── Current ──────────────────────────────────────────────────────────
    JsonObject cur         = doc["current"].as<JsonObject>();
    g_current.temp         = cur["temperature_2m"].as<float>();
    g_current.feels_like   = cur["apparent_temperature"].as<float>();
    g_current.humidity     = cur["relative_humidity_2m"].as<int>();
    g_current.wind_speed   = cur["wind_speed_10m"].as<float>();
    g_current.wind_dir     = cur["wind_direction_10m"].as<int>();
    g_current.weather_code = cur["weather_code"].as<int>();
    g_current.pressure     = cur["surface_pressure"].as<float>();
    g_current.visibility   = cur["visibility"].as<float>() / 1609.34f;  // m → miles
    g_current.uv_index     = cur["uv_index"].as<float>();
    g_current.valid = true;

    // ── Today hi/lo from daily[0] ─────────────────────────────────────────
    g_current.today_max = doc["daily"]["temperature_2m_max"][0] | 0.0f;
    g_current.today_min = doc["daily"]["temperature_2m_min"][0] | 0.0f;

    // ── Hourly: find current hour index, grab HOURLY_COUNT entries ────────
    // Use JsonArrayConst — ArduinoJson v7 MemberProxy is non-copyable, no auto
    JsonArrayConst hourlyTime  = doc["hourly"]["time"].as<JsonArrayConst>();
    JsonArrayConst hourlyTemp  = doc["hourly"]["temperature_2m"].as<JsonArrayConst>();
    JsonArrayConst hourlyCode  = doc["hourly"]["weather_code"].as<JsonArrayConst>();
    JsonArrayConst hourlyPrec  = doc["hourly"]["precipitation_probability"].as<JsonArrayConst>();

    // Find first entry >= current time
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    int curHour = lt->tm_hour;

    int startIdx = 0;
    for (int i = 0; i < (int)hourlyTime.size(); i++) {
        const char *ts = hourlyTime[i] | "";
        if (parseHour(ts) >= curHour) { startIdx = i; break; }
    }

    JsonArrayConst hourlyWind = doc["hourly"]["wind_speed_10m"].as<JsonArrayConst>();
    JsonArrayConst hourlyHumi = doc["hourly"]["relative_humidity_2m"].as<JsonArrayConst>();

    for (int i = 0; i < HOURLY_COUNT; i++) {
        int idx = startIdx + i;
        const char *ts = hourlyTime[idx] | "T00:00";
        g_hourly[i].hour         = parseHour(ts);
        g_hourly[i].temp         = hourlyTemp[idx]  | 0.0f;
        g_hourly[i].weather_code = hourlyCode[idx]  | 0;
        g_hourly[i].precip_prob  = hourlyPrec[idx]  | 0;
        g_hourly[i].wind_speed   = hourlyWind[idx]  | 0.0f;
        g_hourly[i].humidity     = hourlyHumi[idx]  | 0;
    }

    // ── Daily: DAILY_COUNT days ───────────────────────────────────────────
    JsonArrayConst dailyDate    = doc["daily"]["time"].as<JsonArrayConst>();
    JsonArrayConst dailyCode    = doc["daily"]["weather_code"].as<JsonArrayConst>();
    JsonArrayConst dailyMax     = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
    JsonArrayConst dailyMin     = doc["daily"]["temperature_2m_min"].as<JsonArrayConst>();
    JsonArrayConst dailyPrec    = doc["daily"]["precipitation_probability_max"].as<JsonArrayConst>();
    JsonArrayConst dailyWind    = doc["daily"]["wind_speed_10m_max"].as<JsonArrayConst>();
    JsonArrayConst dailyHumi    = doc["daily"]["relative_humidity_2m_max"].as<JsonArrayConst>();
    JsonArrayConst dailySunrise = doc["daily"]["sunrise"].as<JsonArrayConst>();
    JsonArrayConst dailySunset  = doc["daily"]["sunset"].as<JsonArrayConst>();

    for (int i = 0; i < DAILY_COUNT; i++) {
        parseDayName(dailyDate[i] | "", g_daily[i].day);
        g_daily[i].weather_code  = dailyCode[i] | 0;
        g_daily[i].temp_max      = dailyMax[i]  | 0.0f;
        g_daily[i].temp_min      = dailyMin[i]  | 0.0f;
        g_daily[i].precip_prob   = dailyPrec[i] | 0;
        g_daily[i].wind_speed    = dailyWind[i] | 0.0f;
        g_daily[i].humidity_max  = dailyHumi[i] | 0;
        parseSunTime(dailySunrise[i] | "", g_daily[i].sunrise, sizeof(g_daily[i].sunrise));
        parseSunTime(dailySunset[i]  | "", g_daily[i].sunset,  sizeof(g_daily[i].sunset));
    }

    fetchNwsAlert(lat, lon);
    g_weatherUpdatedEpoch = (unsigned long)time(nullptr);
    return true;
}
