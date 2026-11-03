#pragma once
#include <cstdint>

enum class SwipeDir { None, Left, Right, Up, Down };
enum class TapEvent { None, Tap };

struct TouchEvent {
    SwipeDir swipe = SwipeDir::None;
    TapEvent tap   = TapEvent::None;
    int16_t  tapX  = 0;
    int16_t  tapY  = 0;
    // Set when this tap landed soon after, and close to, the previous tap.
    // The tap is still reported as TapEvent::Tap, so callers that count taps
    // (triple-tap reboot, settings favorites) keep seeing every tap.
    bool     doubleTap = false;
};

void       touchInit();
TouchEvent touchPoll();
bool       touchIsHeld(int16_t *ox = nullptr, int16_t *oy = nullptr);
void       touchSetRotation(int rotation);   // 0-3, NVS-backed
int        touchGetRotation();
bool       isRotationPaused();
void       toggleRotationPause();
