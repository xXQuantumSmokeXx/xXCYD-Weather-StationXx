#pragma once

struct Location {
    float lat        = 0;
    float lon        = 0;
    char  city[32]   = {};
    char  region[32] = {};
    char  tz[48]     = {};
    int   utcOffset  = 0;   // seconds, from ip-api "offset" field
    bool  valid      = false;
};

extern Location g_location;

bool locationFetch();   // hits ip-api.com, caches in NVS
void locationLoad();    // loads NVS cache into g_location
