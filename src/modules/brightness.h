#pragma once

#define BRI_LEVELS 6   // 0=auto (LDR), 1-5=fixed dim→max

void brightnessInit();          // call once in setup() — inits LEDC channel and loads NVS
void brightnessSetLevel(int n); // 0=auto, 1-5=fixed; persisted to NVS
int  brightnessGetLevel();      // current level 0-5
void brightnessAutoUpdate();    // call in loop(); only runs when level==0
int  batteryPct();              // 0-100 if LiPo detected on GPIO35, else -1
