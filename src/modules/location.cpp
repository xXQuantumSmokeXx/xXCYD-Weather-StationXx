#include "location.h"
#include "../config/nvs_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <cstring>

Location g_location;

// ── Display alias ─────────────────────────────────────────────────────────
// The city shows in every top bar and lands in every screenshot, so the
// published gallery needs a name that isn't the home address. The alias
// replaces the label only — lat/lon, timezone and API calls keep the real
// location in NVS.
void locationApplyAlias() {
    char alias[sizeof(g_location.city)] = {};
    nvsGetStr("city_alias", alias, sizeof(alias));
    if (!alias[0]) return;
    strncpy(g_location.city, alias, sizeof(g_location.city) - 1);
    g_location.city[sizeof(g_location.city) - 1] = '\0';
}

void locationSetAlias(const char *alias) {
    nvsPutStr("city_alias", alias ? alias : "");
    if (alias && alias[0]) {
        strncpy(g_location.city, alias, sizeof(g_location.city) - 1);
        g_location.city[sizeof(g_location.city) - 1] = '\0';
    } else {
        nvsGetStr("loc_city", g_location.city, sizeof(g_location.city));
    }
}

void locationLoad() {
    g_location.lat   = nvsGetFloat("loc_lat", 0);
    g_location.lon   = nvsGetFloat("loc_lon", 0);
    nvsGetStr("loc_city",   g_location.city,   sizeof(g_location.city));
    nvsGetStr("loc_region", g_location.region, sizeof(g_location.region));
    nvsGetStr("loc_tz",     g_location.tz,     sizeof(g_location.tz));
    g_location.utcOffset = nvsGetInt("loc_offset", 0);
    g_location.valid = (g_location.lat != 0 || g_location.lon != 0);
    locationApplyAlias();
}

bool locationFetch() {
    // Cloudflare already knows where the request came from (cf.latitude etc.),
    // so the Worker answers with the caller's own coordinates — no third-party
    // geo service involved.
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://quantum-meteor.qsmoke.workers.dev/geo");
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, ss.c_str())) return false;

    float lat = doc["lat"] | 0.0f;
    float lon = doc["lon"] | 0.0f;
    if (lat == 0.0f && lon == 0.0f) return false;

    g_location.lat = lat;
    g_location.lon = lon;
    strncpy(g_location.city,   doc["city"]   | "", sizeof(g_location.city)   - 1);
    strncpy(g_location.region, doc["region"] | "", sizeof(g_location.region) - 1);
    strncpy(g_location.tz,     doc["tz"]     | "", sizeof(g_location.tz)     - 1);
    g_location.utcOffset = doc["offset"] | 0;
    g_location.valid = true;

    nvsPutFloat("loc_lat",    g_location.lat);
    nvsPutFloat("loc_lon",    g_location.lon);
    nvsPutStr("loc_city",     g_location.city);
    nvsPutStr("loc_region",   g_location.region);
    nvsPutStr("loc_tz",       g_location.tz);
    nvsPutInt("loc_offset",   g_location.utcOffset);
    locationApplyAlias();      // a refresh must not undo the display name
    return true;
}
