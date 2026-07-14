#include "screen_fireteam.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../modules/weather.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <cstdio>
#include <cstring>
#include <time.h>

namespace {
enum Risk : uint8_t { RISK_LOW, RISK_ELEVATED, RISK_HIGH, RISK_CRITICAL };
struct Assessment { Risk overall, fire, storm, heat; int peakHour, startHour, endHour; float peakTemp, peakWind; int peakRh, peakRain, peakCode; bool official; char reason[88]; };
const char *name(Risk r) { static const char *n[]={"LOW","ELEVATED","HIGH","CRITICAL"}; return n[r]; }
uint16_t color(Risk r) { return r==RISK_LOW ? 0x07E0u : r==RISK_ELEVATED ? COL_AMBER : r==RISK_HIGH ? 0xFBE0u : COL_RED; }
uint16_t aqiColor(int aqi) {
    if (aqi < 0)   return COL_DIM;
    if (aqi <= 50) return 0x07E0u;      // Green — Good
    if (aqi <= 100) return COL_AMBER;    // Amber — Moderate
    if (aqi <= 150) return 0xFBE0u;     // Orange — USG
    if (aqi <= 200) return COL_RED;      // Red — Unhealthy
    if (aqi <= 300) return 0x9810u;     // Purple — Very Unhealthy
    return 0x8000u;                      // Maroon — Hazardous
}
const char *aqiLabel(int aqi) {
    if (aqi < 0)   return "--";
    if (aqi <= 50)  return "Good";
    if (aqi <= 100) return "Moderate";
    if (aqi <= 150) return "Unhealthy";
    if (aqi <= 200) return "Very Unhlthy";
    if (aqi <= 300) return "Hazardous";
    return "Severe";
}
Risk fireRisk(int rh, float wind, int rain) {
    int s=(rh<20?3:rh<30?2:rh<40?1:0)+(wind>=30?3:wind>=20?2:wind>=12?1:0)+(rain<15?1:0);
    return s>=6?RISK_CRITICAL:s>=4?RISK_HIGH:s>=2?RISK_ELEVATED:RISK_LOW;
}
Risk stormRisk(int code, float wind, int rain) {
    return (code>=95||wind>=35)?RISK_CRITICAL:(code>=80||wind>=25||rain>=75)?RISK_HIGH:(code>=51||wind>=15||rain>=45)?RISK_ELEVATED:RISK_LOW;
}
Risk heatRisk(float temp, int rh) {
    float hi=temp;
    if(temp>=80&&rh>=40){float t2=temp*temp,r2=rh*rh;hi=-42.379+2.04901523*temp+10.14333127*rh-.22475541*temp*rh-.00683783*t2-.05481717*r2+.00122874*t2*rh+.00085282*temp*r2-.00000199*t2*r2;}
    return hi>=125?RISK_CRITICAL:hi>=105?RISK_HIGH:hi>=95?RISK_ELEVATED:RISK_LOW;
}
Assessment assess() {
    Assessment a = {};
    a.fire = fireRisk(g_current.humidity, g_current.wind_speed, g_hourly[0].precip_prob);
    a.storm = stormRisk(g_current.weather_code, g_current.wind_speed, g_hourly[0].precip_prob);
    a.heat = heatRisk(g_current.temp, g_current.humidity);
    a.overall = (Risk)max((int)a.fire, max((int)a.storm, (int)a.heat));
    a.peakHour = a.startHour = a.endHour = g_hourly[0].hour;
    a.peakTemp = g_current.temp;
    a.peakWind = g_current.wind_speed;
    a.peakRh = g_current.humidity;
    a.peakRain = g_hourly[0].precip_prob;
    a.peakCode = g_current.weather_code;

    for (int i = 0; i < HOURLY_COUNT; i++) {
        Risk fire = fireRisk(g_hourly[i].humidity, g_hourly[i].wind_speed, g_hourly[i].precip_prob);
        Risk storm = stormRisk(g_hourly[i].weather_code, g_hourly[i].wind_speed, g_hourly[i].precip_prob);
        Risk heat = heatRisk(g_hourly[i].temp, g_hourly[i].humidity);
        Risk combined = (Risk)max((int)fire, max((int)storm, (int)heat));
        if (fire > a.fire) a.fire = fire;
        if (storm > a.storm) a.storm = storm;
        if (heat > a.heat) a.heat = heat;
        if (combined > a.overall || (combined == a.overall && g_hourly[i].temp > a.peakTemp)) {
            a.overall = combined;
            a.peakHour = g_hourly[i].hour;
            a.peakTemp = g_hourly[i].temp;
            a.peakWind = g_hourly[i].wind_speed;
            a.peakRh = g_hourly[i].humidity;
            a.peakRain = g_hourly[i].precip_prob;
            a.peakCode = g_hourly[i].weather_code;
        }
    }
    a.overall = (Risk)max((int)a.fire, max((int)a.storm, (int)a.heat));

    bool foundWindow = false;
    for (int i = 0; i < HOURLY_COUNT; i++) {
        Risk combined = (Risk)max(
            (int)fireRisk(g_hourly[i].humidity, g_hourly[i].wind_speed, g_hourly[i].precip_prob),
            max((int)stormRisk(g_hourly[i].weather_code, g_hourly[i].wind_speed, g_hourly[i].precip_prob),
                (int)heatRisk(g_hourly[i].temp, g_hourly[i].humidity)));
        if (combined == a.overall) {
            if (!foundWindow) { a.startHour = g_hourly[i].hour; foundWindow = true; }
            a.endHour = g_hourly[i].hour;
        }
    }

    if (g_weatherAlert.active) {
        Risk alertRisk = RISK_ELEVATED;
        if (!strcmp(g_weatherAlert.severity, "Extreme") || !strcmp(g_weatherAlert.severity, "Severe")) alertRisk = RISK_CRITICAL;
        else if (!strcmp(g_weatherAlert.severity, "Moderate")) alertRisk = RISK_HIGH;
        if (strstr(g_weatherAlert.event, "Heat")) a.heat = (Risk)max((int)a.heat, (int)alertRisk);
        else if (strstr(g_weatherAlert.event, "Fire") || strstr(g_weatherAlert.event, "Red Flag")) a.fire = (Risk)max((int)a.fire, (int)alertRisk);
        else a.storm = (Risk)max((int)a.storm, (int)alertRisk);
        a.overall = (Risk)max((int)a.fire, max((int)a.storm, (int)a.heat));
        a.official = true;
        snprintf(a.reason, sizeof(a.reason), "NWS ALERT: %.43s", g_weatherAlert.event);
    } else if (a.heat == a.overall && a.heat >= RISK_ELEVATED) {
        snprintf(a.reason, sizeof(a.reason), "Peak heat %.0f F  Humidity %d%% near %d %s", a.peakTemp, a.peakRh, a.peakHour % 12 ? a.peakHour % 12 : 12, a.peakHour < 12 ? "AM" : "PM");
    } else if (a.fire == a.overall && a.fire >= RISK_ELEVATED) {
        snprintf(a.reason, sizeof(a.reason), "Dry air %d%%  Wind %.0f mph  Rain %d%%", a.peakRh, a.peakWind, a.peakRain);
    } else if (a.storm == a.overall && a.storm >= RISK_ELEVATED) {
        snprintf(a.reason, sizeof(a.reason), "%s  Wind %.0f mph  Rain %d%%", wmoDescription(a.peakCode), a.peakWind, a.peakRain);
    } else {
        snprintf(a.reason, sizeof(a.reason), "No elevated threats in the next 12 hours");
    }
    return a;
}
void formatHour(int hour, char *out, size_t size) {
    snprintf(out, size, "%d %s", hour % 12 ? hour % 12 : 12, hour < 12 ? "AM" : "PM");
}

void formatUpdated(char *out, size_t size) {
    if (!g_weatherUpdatedEpoch) { snprintf(out, size, "UPDATED --"); return; }
    time_t stamp = (time_t)g_weatherUpdatedEpoch;
    struct tm *lt = localtime(&stamp);
    int h = lt->tm_hour;
    snprintf(out, size, "UPDATED %d:%02d%s", h % 12 ? h % 12 : 12, lt->tm_min, h < 12 ? "A" : "P");
}
const char *guidance(const Assessment &a) {
    // Prioritize immediate life-safety hazards when levels are tied.
    if (a.storm == a.overall && a.storm >= RISK_ELEVATED) {
        if (a.storm == RISK_CRITICAL) return "Seek substantial shelter; avoid windows.";
        if (a.storm == RISK_HIGH) return "Move activities indoors before storms.";
        return "Monitor conditions; secure loose items.";
    }
    if (a.heat == a.overall && a.heat >= RISK_ELEVATED) {
        if (a.heat == RISK_CRITICAL) return "Avoid exertion; seek cooled shelter.";
        if (a.heat == RISK_HIGH) return "Hydrate often; take cooling breaks.";
        return "Hydrate regularly; limit hard activity.";
    }
    if (a.fire == a.overall && a.fire >= RISK_ELEVATED) {
        if (a.fire == RISK_CRITICAL) return "Avoid ignition sources; track alerts.";
        if (a.fire == RISK_HIGH) return "Postpone burning; fire may spread fast.";
        return "Avoid sparks and unattended flames.";
    }
    return "No special precautions needed right now.";
}
void card(TFT_eSPI&t,int x,const char*label,Risk r){uint16_t c=color(r);t.fillRoundRect(x,104,96,48,4,COL_BG);t.drawRoundRect(x,104,96,48,4,g_themeColor);t.setTextFont(FONT_SM);t.setTextColor(g_themeColor,COL_BG);t.setCursor(x+7,111);t.print(label);t.setTextFont(FONT_MD);t.setTextColor(c,COL_BG);t.setCursor(x+7,128);t.print(name(r));}
}
void screenFireteamDraw(TFT_eSPI &tft, bool wifiOk) {
    char ts[10];timeGetShort(ts);drawTopbar(tft,g_location.valid?g_location.city:"","FIRETEAM",ts,wifiOk);char dateStr[28];timeGetDateLong(dateStr,sizeof(dateStr));drawBottombar(tft,dateStr,4,12);tft.fillRect(0,CONTENT_Y,SCREEN_W,CONTENT_H,COL_BG);
    if(!g_current.valid){tft.setTextFont(FONT_MD);tft.setTextColor(g_themeColor,COL_BG);tft.setCursor(74,102);tft.print(wifiOk?"Awaiting weather data":"Fireteam offline");return;}
    Assessment a=assess();uint16_t c=color(a.overall);tft.setTextFont(FONT_SM);tft.setTextColor(g_themeColor,COL_BG);tft.setCursor(10,31);tft.print("READINESS LEVEL");tft.setTextFont(FONT_LG);tft.setTextColor(c,COL_BG);tft.setCursor(10,46);tft.print(name(a.overall));
    // Air Quality — right-aligned, same y as readiness
    {tft.setTextFont(FONT_SM);tft.setTextColor(g_themeColor,COL_BG);int w=tft.textWidth("AIR QUALITY");tft.setCursor(SCREEN_W-w-10,31);tft.print("AIR QUALITY");tft.setTextFont(FONT_LG);uint16_t aq=aqiColor(g_current.aqi);tft.setTextColor(aq,COL_BG);char aqs[8];snprintf(aqs,sizeof(aqs),g_current.aqi>=0?"%d":"--",g_current.aqi);int aw=tft.textWidth(aqs);tft.setCursor(SCREEN_W-aw-10,46);tft.print(aqs);}
    tft.drawFastHLine(10,78,300,g_themeColor);tft.setTextFont(FONT_SM);tft.setTextColor(COL_WHITE,COL_BG);tft.setCursor(10,86);tft.print(a.reason);
    card(tft,8,"FIRE",a.fire);card(tft,112,"STORM",a.storm);card(tft,216,"HEAT",a.heat);
    char updated[24], start[10], end[10];
    formatUpdated(updated, sizeof(updated));
    formatHour(a.startHour, start, sizeof(start));
    formatHour(a.endHour, end, sizeof(end));
    tft.setTextColor(g_themeColor, COL_BG); tft.setCursor(10,166); tft.print(updated);
    tft.setCursor(226,166);
    if (a.official) { tft.setTextColor(COL_RED, COL_BG); tft.print("NWS ACTIVE"); }
    else if (g_alertsChecked) { tft.setTextColor(g_themeColor, COL_BG); tft.print("NWS OK"); }
    else { tft.setTextColor(COL_AMBER, COL_BG); tft.print("NWS ?"); }
    tft.setTextColor(c, COL_BG); tft.setCursor(10,181);
    if (a.official) tft.print("Official alert overrides forecast risk");
    else if (a.overall == RISK_LOW) tft.print("No elevated threat window");
    else if (a.startHour == a.endHour) tft.printf("Highest risk near %s", start);
    else tft.printf("Highest risk %s - %s", start, end);
    tft.setTextColor(g_themeColor, COL_BG); tft.setCursor(10,198); tft.print(guidance(a));
}
