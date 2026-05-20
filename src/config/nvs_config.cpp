#include "nvs_config.h"
#include <Preferences.h>
#include <cstring>

static Preferences prefs;

void nvsInit() {
    prefs.begin("cydwx", false);
}

void nvsPutInt(const char *key, int32_t val) {
    prefs.putInt(key, val);
}

int32_t nvsGetInt(const char *key, int32_t def) {
    return prefs.getInt(key, def);
}

void nvsPutStr(const char *key, const char *val) {
    prefs.putString(key, val);
}

void nvsGetStr(const char *key, char *buf, size_t len, const char *def) {
    String s = prefs.getString(key, def);
    strncpy(buf, s.c_str(), len - 1);
    buf[len - 1] = '\0';
}

void nvsPutFloat(const char *key, float val) {
    prefs.putFloat(key, val);
}

float nvsGetFloat(const char *key, float def) {
    return prefs.getFloat(key, def);
}
