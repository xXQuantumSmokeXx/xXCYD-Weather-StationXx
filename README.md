# xXCYD-Weather-StationXx

Tactical weather station for the ESP32-2432S028 (CYD — Cheap Yellow Display).

![NOW Screen](ScreenShots/screen_now.png)

---

| Hourly | 5-Day Forecast | Settings |
|--------|---------------|----------|
| ![Hourly](ScreenShots/screen_hourly.png) | ![Forecast](ScreenShots/screen_forecast.png) | ![Settings](ScreenShots/screen_settings.png) |

---

### Features

- Current temperature, HI/LO, feels like, UV, humidity, wind, pressure, visibility
- Moon phase disk with illumination % and phase name, sunrise/sunset
- 12-hour hourly forecast and 5-day forecast
- Auto-detects location — no config needed beyond WiFi credentials
- Weather data via Open-Meteo (no API key required)
- Refreshes at the top of each hour
- Auto-rotate cycles screens at configurable interval
- 9 theme accent colors, brightness control — all saved across reboots

### Setup

1. Copy `src/config/secrets.h.example` to `src/config/secrets.h` and add your WiFi credentials
2. Flash `firmware.bin` from the release, or build with PlatformIO

---

*by xXMayDayXx*
