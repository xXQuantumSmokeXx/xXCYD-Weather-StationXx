#pragma once
#include <cstdint>
#include <cstddef>

void     nvsInit();
void     nvsPutInt(const char *key, int32_t val);
int32_t  nvsGetInt(const char *key, int32_t def = 0);
void     nvsPutStr(const char *key, const char *val);
void     nvsGetStr(const char *key, char *buf, size_t len, const char *def = "");
void     nvsPutFloat(const char *key, float val);
float    nvsGetFloat(const char *key, float def = 0.0f);
