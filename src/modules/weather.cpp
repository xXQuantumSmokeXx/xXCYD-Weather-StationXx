#include "weather.h"
#include "time_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

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
static bool weatherFetchNWS(float lat, float lon);
static bool weatherFetchNWSPublic(float lat, float lon);

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
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    http.addHeader("Accept-Encoding", "identity");  // prevent gzip — ArduinoJson can't decompress
    int code = http.GET();
    if (code != 200) {
        g_weatherError = code;
        http.end();
        return weatherFetchNWS(lat, lon);
    }
    g_weatherError = 0;

    String body = http.getString();
    http.end();

    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        g_weatherError = -(int)err.code();  // negative = JSON error
        return weatherFetchNWS(lat, lon);
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

// ═══════════════════════════════════════════════════════════════════════════
// NWS API fallback — api.weather.gov, free, no key, HTTPS (.gov = ESP32 compat)
// ═══════════════════════════════════════════════════════════════════════════

// Map "10 mph" → float
static float nwsWindSpeed(const char *s) {
    if (!s) return 0;
    return strtof(s, nullptr);
}

// Map "NW" → degrees
static int nwsWindDir(const char *s) {
    if (!s || !s[0]) return 0;
    char buf[4]; strncpy(buf, s, 3); buf[3] = 0;
    for (char *p = buf; *p; p++) *p = tolower(*p);
    if (strcmp(buf, "n") == 0)  return 0;
    if (strcmp(buf, "ne") == 0) return 45;
    if (strcmp(buf, "e") == 0)  return 90;
    if (strcmp(buf, "se") == 0) return 135;
    if (strcmp(buf, "s") == 0)  return 180;
    if (strcmp(buf, "sw") == 0) return 225;
    if (strcmp(buf, "w") == 0)  return 270;
    if (strcmp(buf, "nw") == 0) return 315;
    if (strcmp(buf, "nnw")==0)  return 338;
    if (strcmp(buf, "wsw")==0)  return 248;
    if (strcmp(buf, "ene")==0)  return 68;
    if (strcmp(buf, "nne")==0)  return 23;
    if (strcmp(buf, "wnw")==0)  return 293;
    if (strcmp(buf, "ese")==0)  return 113;
    if (strcmp(buf, "sse")==0)  return 158;
    if (strcmp(buf, "ssw")==0)  return 203;
    return 0;
}

// Map NWS shortForecast to WMO code
static int nwsToWmo(const char *fc) {
    if (!fc || !fc[0]) return 0;
    char buf[64]; strncpy(buf, fc, 63); buf[63] = 0;
    for (char *p = buf; *p; p++) *p = tolower(*p);
    if (strstr(buf, "thunderstorm") || strstr(buf, "t-storm")) return 95;
    if (strstr(buf, "heavy snow") || strstr(buf, "blizzard")) return 75;
    if (strstr(buf, "snow")) return 71;
    if (strstr(buf, "freezing rain") || strstr(buf, "sleet")) return 56;
    if (strstr(buf, "heavy rain")) return 65;
    if (strstr(buf, "rain") && strstr(buf, "shower")) return 80;
    if (strstr(buf, "rain")) return 61;
    if (strstr(buf, "drizzle")) return 51;
    if (strstr(buf, "fog") || strstr(buf, "haze")) return 45;
    if (strstr(buf, "overcast")) return 3;
    if (strstr(buf, "mostly cloudy")) return 3;
    if (strstr(buf, "partly cloudy")) return 2;
    if (strstr(buf, "mostly sunny") || strstr(buf, "mostly clear")) return 2;
    if (strstr(buf, "partly sunny")) return 2;
    if (strstr(buf, "sunny") || strstr(buf, "clear") || strstr(buf, "fair")) return 0;
    if (strstr(buf, "cloudy")) return 3;
    return 3;
}

// NWS GET helper — explicit host/path avoids HTTPClient URL parsing bugs
static bool nwsGet(const char *host, const char *path, JsonDocument &doc) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    HTTPClient http;
    if (!http.begin(client, host, 443, path, true)) { http.end(); return false; }
    http.setTimeout(12000);
    http.addHeader("User-Agent", "xXCYD-Weather-StationXx github.com/xXQuantumSmokeXx");
    http.addHeader("Accept", "application/geo+json");
    http.addHeader("Accept-Encoding", "identity");
    int code = http.GET();
    if (code != 200) {
        g_weatherError = code;
        http.end();
        return false;
    }
    g_weatherError = 0;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) { g_weatherError = -(int)err.code(); return false; }
    return true;
}

// Strip "https://" prefix, return pointer to host, and set *path to the path part
static const char* splitUrl(const char *url, const char **path) {
    if (!url) return nullptr;
    const char *p = strstr(url, "://");
    if (!p) return nullptr;
    p += 3;
    const char *slash = strchr(p, '/');
    if (slash) {
        *path = slash;
        // We need a mutable copy for the host. Caller must provide a buffer.
        return p;  // caller must strndup this
    }
    *path = "/";
    return p;
}

static bool weatherFetchNWS(float lat, float lon) {
    // Step 1: Get grid endpoint URLs
    char path[96];
    snprintf(path, sizeof(path), "/points/%.4f,%.4f", lat, lon);
    JsonDocument ptsDoc;
    if (!nwsGet("api.weather.gov", path, ptsDoc)) return weatherFetchNWSPublic(lat, lon);

    const char *forecastUrl = ptsDoc["properties"]["forecast"] | "";
    const char *hourlyUrl   = ptsDoc["properties"]["forecastHourly"] | "";
    if (!forecastUrl[0] || !hourlyUrl[0]) { g_weatherError = -100; return weatherFetchNWSPublic(lat, lon); }

    // Parse NWS return URLs to get host + path for subsequent calls
    const char *fcPath, *hrPath, *nwsHost;
    char hostBuf[64];
    nwsHost = splitUrl(forecastUrl, &fcPath);
    if (!nwsHost) return weatherFetchNWSPublic(lat, lon);
    size_t hlen = strlen(nwsHost);
    if (hlen > 63) hlen = 63;
    memcpy(hostBuf, nwsHost, hlen); hostBuf[hlen] = 0;
    splitUrl(hourlyUrl, &hrPath);  // same host, just get path

    // Step 2: Hourly forecast → current + hourly
    JsonDocument hDoc;
    if (!nwsGet(hostBuf, hrPath, hDoc)) return weatherFetchNWSPublic(lat, lon);

    JsonArrayConst periods = hDoc["properties"]["periods"].as<JsonArrayConst>();
    int pc = (int)periods.size();
    if (pc == 0) { g_weatherError = -101; return weatherFetchNWSPublic(lat, lon); }

    JsonObjectConst p0 = periods[0].as<JsonObjectConst>();
    g_current.temp         = p0["temperature"].as<float>();
    g_current.wind_speed   = nwsWindSpeed(p0["windSpeed"] | "0");
    g_current.wind_dir     = nwsWindDir(p0["windDirection"] | "N");
    g_current.weather_code = nwsToWmo(p0["shortForecast"] | "");
    g_current.humidity     = (int)(p0["relativeHumidity"]["value"].as<float>() + 0.5f);
    g_current.feels_like   = g_current.temp;
    g_current.pressure     = 0;
    g_current.visibility   = 0;
    g_current.uv_index     = 0;
    g_current.today_max    = g_current.temp;
    g_current.today_min    = g_current.temp;
    g_current.valid        = true;

    for (int i = 0; i < HOURLY_COUNT && i < pc; i++) {
        JsonObjectConst p = periods[i].as<JsonObjectConst>();
        g_hourly[i].hour         = parseHour(p["startTime"] | "");
        g_hourly[i].temp         = p["temperature"].as<float>();
        g_hourly[i].weather_code = nwsToWmo(p["shortForecast"] | "");
        g_hourly[i].precip_prob  = (int)(p["probabilityOfPrecipitation"]["value"].as<float>() + 0.5f);
        g_hourly[i].wind_speed   = nwsWindSpeed(p["windSpeed"] | "0");
        g_hourly[i].humidity     = (int)(p["relativeHumidity"]["value"].as<float>() + 0.5f);
    }

    // Step 3: Daily forecast
    JsonDocument dDoc;
    if (!nwsGet(hostBuf, fcPath, dDoc)) return weatherFetchNWSPublic(lat, lon);

    JsonArrayConst days = dDoc["properties"]["periods"].as<JsonArrayConst>();
    int dc = (int)days.size(), di = 0;
    for (int i = 0; i < dc && di < DAILY_COUNT; i++) {
        JsonObjectConst dp = days[i].as<JsonObjectConst>();
        if (!(dp["isDaytime"] | false)) continue;

        g_daily[di].weather_code = nwsToWmo(dp["shortForecast"] | "");
        g_daily[di].temp_max     = dp["temperature"].as<float>();
        g_daily[di].wind_speed   = nwsWindSpeed(dp["windSpeed"] | "0");
        g_daily[di].precip_prob  = (int)(dp["probabilityOfPrecipitation"]["value"].as<float>() + 0.5f);
        g_daily[di].humidity_max = (int)(dp["relativeHumidity"]["value"].as<float>() + 0.5f);

        if (i + 1 < dc) {
            JsonObjectConst np = days[i + 1].as<JsonObjectConst>();
            if (!(np["isDaytime"] | true))
                g_daily[di].temp_min = np["temperature"].as<float>();
            else g_daily[di].temp_min = g_daily[di].temp_max;
        } else g_daily[di].temp_min = g_daily[di].temp_max;

        parseDayName(dp["startTime"] | "", g_daily[di].day);
        parseSunTime(dp["startTime"] | "", g_daily[di].sunrise, sizeof(g_daily[di].sunrise));
        parseSunTime(dp["endTime"]   | "", g_daily[di].sunset,  sizeof(g_daily[di].sunset));

        if (di == 0) {
            if (g_daily[0].temp_max > g_current.today_max) g_current.today_max = g_daily[0].temp_max;
            if (g_daily[0].temp_min < g_current.today_min)  g_current.today_min  = g_daily[0].temp_min;
        }
        di++;
    }

    fetchNwsAlert(lat, lon);
    g_weatherUpdatedEpoch = (unsigned long)time(nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// NWS Public forecast.weather.gov — third fallback (different CDN than api.weather.gov)
// Single HTTPS call: current observations + 14-period (7-day) forecast
// ═══════════════════════════════════════════════════════════════════════════
static bool weatherFetchNWSPublic(float lat, float lon) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://forecast.weather.gov/MapClick.php?lat=%.4f&lon=%.4f&unit=0&lg=english&FcstType=json",
        lat, lon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) { http.end(); return false; }
    http.setTimeout(15000);
    http.addHeader("User-Agent", "xXCYD-Weather-StationXx github.com/xXQuantumSmokeXx");
    http.addHeader("Accept-Encoding", "identity");
    int code = http.GET();
    if (code != 200) { g_weatherError = code; http.end(); return false; }
    g_weatherError = 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) { g_weatherError = -(int)err.code(); return false; }

    JsonObjectConst obs = doc["currentobservation"].as<JsonObjectConst>();
    if (obs.isNull()) { g_weatherError = -110; return false; }

    g_current.temp         = atof(obs["Temp"] | "0");
    g_current.humidity     = atoi(obs["Relh"] | "0");
    g_current.wind_speed   = atof(obs["Winds"] | "0");
    g_current.wind_dir     = atoi(obs["Windd"] | "0");
    g_current.weather_code = nwsToWmo(obs["Weather"] | "");
    g_current.visibility   = atof(obs["Visibility"] | "0");
    g_current.pressure     = atof(obs["SLP"] | "0") * 33.8639f;
    g_current.feels_like   = g_current.temp;
    const char *windChill = obs["WindChill"] | "";
    if (windChill[0] && strcmp(windChill, "NA") != 0)
        g_current.feels_like = atof(windChill);

    g_current.uv_index  = 0;
    g_current.today_max = g_current.temp;
    g_current.today_min = g_current.temp;
    g_current.valid     = true;

    JsonArrayConst temps = doc["data"]["temperature"].as<JsonArrayConst>();
    JsonArrayConst pops  = doc["data"]["pop"].as<JsonArrayConst>();
    JsonArrayConst wx    = doc["data"]["weather"].as<JsonArrayConst>();
    JsonArrayConst times = doc["time"]["startValidTime"].as<JsonArrayConst>();

    int tc = (int)temps.size();

    // NWS periods alternate: 0=Tonight, 1=Tomorrow, 2=Tomorrow night, 3=Day2...
    // NOW screen "today": tonight's low + tomorrow's high (next 24h)
    g_current.today_max = atof(temps[1] | "0");
    g_current.today_min = atof(temps[0] | "0");

    // 5-Day: period 1+2 = day0, 3+4 = day1, 5+6 = day2, etc.
    // Each entry = daytime high + following night low
    for (int d = 0; d < DAILY_COUNT && (d * 2 + 1) < tc; d++) {
        int dayIdx   = d * 2 + 1;
        int nightIdx = d * 2 + 2;

        g_daily[d].temp_max     = atof(temps[dayIdx] | "0");
        g_daily[d].temp_min     = (nightIdx < tc) ? atof(temps[nightIdx] | "0") : g_daily[d].temp_max;
        g_daily[d].weather_code = nwsToWmo(wx[dayIdx] | "");
        parseDayName(times[dayIdx] | "", g_daily[d].day);
        const char *ps = pops[dayIdx] | "";
        g_daily[d].precip_prob  = ps[0] ? atoi(ps) : 0;
        g_daily[d].wind_speed   = 0;
        g_daily[d].humidity_max = 0;
        parseSunTime(times[dayIdx]   | "", g_daily[d].sunrise, sizeof(g_daily[d].sunrise));
        parseSunTime(times[nightIdx] | "", g_daily[d].sunset,  sizeof(g_daily[d].sunset));
    }

    // Hourly: this endpoint has no real hourly data. Backfill with current
    // conditions spread across the next 12h rather than fake per-hour numbers.
    int curHour = 0;
    if (timeIsValid()) {
        time_t now = time(nullptr);
        curHour = localtime(&now)->tm_hour;
    }
    for (int i = 0; i < HOURLY_COUNT; i++) {
        g_hourly[i].hour         = (curHour + i) % 24;
        g_hourly[i].temp         = g_current.temp;
        g_hourly[i].weather_code = g_current.weather_code;
        g_hourly[i].precip_prob  = g_daily[0].precip_prob;
        g_hourly[i].wind_speed   = g_current.wind_speed;
        g_hourly[i].humidity     = g_current.humidity;
    }

    fetchNwsAlert(lat, lon);
    g_weatherUpdatedEpoch = (unsigned long)time(nullptr);
    return true;
}
