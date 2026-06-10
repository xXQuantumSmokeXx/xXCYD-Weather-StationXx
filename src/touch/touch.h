#pragma once
#include <cstdint>

enum class SwipeDir { None, Left, Right, Up, Down };
enum class TapEvent { None, Tap };

struct TouchEvent {
    SwipeDir swipe = SwipeDir::None;
    TapEvent tap   = TapEvent::None;
    int16_t  tapX  = 0;
    int16_t  tapY  = 0;
};

void       touchInit();
TouchEvent touchPoll();
bool       touchIsHeld(int16_t *ox = nullptr, int16_t *oy = nullptr);
void       touchSetRotation(int rotation);   // 0-3, NVS-backed
int        touchGetRotation();
