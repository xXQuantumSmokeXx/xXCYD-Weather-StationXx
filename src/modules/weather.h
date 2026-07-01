#pragma once
#include "../config/config.h"

struct CurrentWeather {
    float temp          = 0;
    float feels_like    = 0;
    int   humidity      = 0;
    float wind_speed    = 0;
    int   wind_dir      = 0;
    int   weather_code  = 0;
    float today_max     = 0;
    float today_min     = 0;
    float pressure      = 0;   // hPa (surface pressure)
    float visibility    = 0;   // miles (converted from meters)
    float uv_index      = 0;
    bool  valid         = false;
};

struct HourlyWeather {
    int   hour          = 0;
    float temp          = 0;
    int   weather_code  = 0;
    int   precip_prob   = 0;
    float wind_speed    = 0;
    int   humidity      = 0;
};

struct DailyWeather {
    char  day[4]        = {};
    int   weather_code  = 0;
    float temp_max      = 0;
    float temp_min      = 0;
    int   precip_prob   = 0;
    float wind_speed    = 0;
    int   humidity_max  = 0;
    char  sunrise[8]    = {};   // e.g. "7:23a"
    char  sunset[8]     = {};   // e.g. "7:45p"
};

extern CurrentWeather   g_current;
extern HourlyWeather    g_hourly[HOURLY_COUNT];
extern DailyWeather     g_daily[DAILY_COUNT];
extern int              g_utcOffsetSec;

struct WeatherAlert {
    bool active = false;
    char event[48] = {};
    char severity[16] = {};
};

extern WeatherAlert g_weatherAlert;
extern bool         g_alertsChecked;
extern unsigned long g_weatherUpdatedEpoch;
extern int              g_weatherError;   // last HTTP error code, 0 = ok

bool        weatherFetch(float lat, float lon);
const char* wmoDescription(int code);
const char* windCardinal(int deg);

// Icon category for drawing
enum class WxIcon { Sun, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Snow, Showers, Storm };
WxIcon wmoIcon(int code);
