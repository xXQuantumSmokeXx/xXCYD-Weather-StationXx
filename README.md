# xXCYD-Weather-StationXx

Tactical weather station firmware for the ESP32-2432S028 (CYD — Cheap Yellow Display) with ILI9341 320×240 touchscreen.

![NOW Screen](ScreenShots/screen_now.png)

---

### Gallery

| Hourly | 5-Day Forecast | Settings |
|--------|---------------|----------|
| ![Hourly](ScreenShots/screen_hourly.png) | ![Forecast](ScreenShots/screen_forecast.png) | ![Settings](ScreenShots/screen_settings.png) |

---

### Features

- **NOW screen** — current temperature (7-segment display), HI/LO, feels like, UV, humidity, wind, pressure, visibility, moon phase, sunrise/sunset
- **Hourly screen** — 12-hour forecast with temperature, precipitation chance, and wind
- **5-Day Forecast** — daily HI/LO, condition icon, precipitation chance, wind
- **Settings screen** — WiFi info, IP, GPS coordinates, battery, brightness control, auto-rotate (OFF/10s/30s/1m/5m), theme color picker
- **Auto-rotate** — cycles NOW → Hourly → Forecast at configurable interval, persisted via NVS
- **Hourly weather refresh** — fetches fresh data at the top of each clock hour via Open-Meteo API
- **Theme colors** — 9 accent colors, saved across reboots
- **Moon phase** — rendered disk with illumination % and phase name
- **Serial screenshot** — capture any screen over USB: `python screenshot.py COM11`

### Hardware

- ESP32-2432S028 (CYD) — ESP32 + ILI9341 320×240 + XPT2046 touch + RGB LED
- Powered via USB-C

### Stack

- PlatformIO / Arduino framework
- TFT_eSPI 2.5.43
- Open-Meteo API (weather, no key required)
- ip-api.com (auto location + UTC offset)
- SNTP (time sync)
- NVS (Preferences) for persistent settings

### Setup

1. Copy `src/config/secrets.h.example` to `src/config/secrets.h` and fill in your WiFi credentials
2. Build and flash with PlatformIO (`pio run -e cyd_weather -t upload`)
3. Device auto-detects location and fetches weather on boot

---

*by xXMayDayXx*
