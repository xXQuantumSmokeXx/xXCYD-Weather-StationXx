#include "location.h"
#include "../config/nvs_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <cstring>

Location g_location;

void locationLoad() {
    g_location.lat   = nvsGetFloat("loc_lat", 0);
    g_location.lon   = nvsGetFloat("loc_lon", 0);
    nvsGetStr("loc_city",   g_location.city,   sizeof(g_location.city));
    nvsGetStr("loc_region", g_location.region, sizeof(g_location.region));
    nvsGetStr("loc_tz",     g_location.tz,     sizeof(g_location.tz));
    g_location.utcOffset = nvsGetInt("loc_offset", 0);
    g_location.valid = (g_location.lat != 0 || g_location.lon != 0);
}

bool locationFetch() {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://ip-api.com/json/?fields=status,lat,lon,city,regionName,timezone,offset");
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument doc;
    auto err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return false;

    const char *status = doc["status"] | "fail";
    if (strcmp(status, "success") != 0) return false;

    g_location.lat = doc["lat"].as<float>();
    g_location.lon = doc["lon"].as<float>();
    strncpy(g_location.city,   doc["city"]       | "", sizeof(g_location.city)   - 1);
    strncpy(g_location.region, doc["regionName"] | "", sizeof(g_location.region) - 1);
    strncpy(g_location.tz,     doc["timezone"]   | "", sizeof(g_location.tz)     - 1);
    g_location.utcOffset = doc["offset"].as<int>();
    g_location.valid = true;

    nvsPutFloat("loc_lat",    g_location.lat);
    nvsPutFloat("loc_lon",    g_location.lon);
    nvsPutStr("loc_city",     g_location.city);
    nvsPutStr("loc_region",   g_location.region);
    nvsPutStr("loc_tz",       g_location.tz);
    nvsPutInt("loc_offset",   g_location.utcOffset);
    return true;
}
