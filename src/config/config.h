#pragma once

// ── Hardware version ─────────────────────────────────────────────────────────
// CYD 2.8" boards have two hardware revisions:
//   1 = ESP32-32E (1-USB, original)               → standard landscape rotation 1
//   2 = 2-USB (newer, 2 USB ports)                 → landscape + mirror Y
// The 2-USB version has the LCD physically flipped compared to the 1-USB.
//
// Default is 1 (ESP32-32E).
// For 2-USB, override in platformio.ini build_flags with -DCYD_USB_VERSION=2
#ifndef CYD_USB_VERSION
#define CYD_USB_VERSION  1
#endif

// ── Display (ILI9341 on HSPI) ─────────────────────────────────────────────
#define TFT_MOSI   13
#define TFT_MISO   12
#define TFT_SCLK   14
#define TFT_CS     15
#define TFT_DC      2
#define TFT_RST    -1
#define TFT_BL     21

// ── Touch (XPT2046 on VSPI — remapped) ───────────────────────────────────
// NOTE: LovyanGFX remaps VSPI to these pins after SD card init.
//       SD must be read at boot before tft.init().
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_SCLK 25
// Calibration — adjust if touches feel offset
#define TOUCH_X_MIN  300
#define TOUCH_X_MAX 3900
#define TOUCH_Y_MIN  200
#define TOUCH_Y_MAX 3800

// ── SD Card (VSPI default pins — used at boot ONLY before touch init) ────
#define SD_CS    5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCLK 18

// ── RGB LED ──────────────────────────────────────────────────────────────
#define LED_R 22
#define LED_G 16
#define LED_B 17

// ── LDR (light sensor for auto-brightness) ───────────────────────────────
#define LDR_PIN 35   // LDR is on GPIO35 on CYD boards (GPIO34 = battery voltage divider)

// ── Screen geometry ───────────────────────────────────────────────────────
#define SCREEN_W   320
#define SCREEN_H   240
#define TOPBAR_H    22
#define BOTBAR_H    22
#define CONTENT_Y  (TOPBAR_H + 1)
#define CONTENT_H  (SCREEN_H - TOPBAR_H - BOTBAR_H - 2)


// ── App settings ─────────────────────────────────────────────────────────
#define HOURLY_COUNT        12
#define DAILY_COUNT          5
#define AP_NAME             "CYD-Weather"
#include "secrets.h"   // gitignored — copy secrets.h.example to secrets.h
