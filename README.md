# xXCYD-Weather-StationXx

A tactical environmental monitoring station for the ESP32-2432S028 (CYD / Cheap Yellow Display), consolidating real-time weather, space weather, wildfire and earthquake tracking, and customizable display profiles into a single portable unit.

[![Support on Patreon](https://img.shields.io/badge/Support-Patreon-orange)](https://www.patreon.com/c/xXQuantumSmokeXx)

## Screens

### NOW
![NOW](ScreenShots/NOW.png)

| HOURLY | 5-DAY | SOLAR |
|--------|-------|-------|
| ![HOURLY](ScreenShots/HOURLY.png) | ![5-DAY](ScreenShots/5-DAY.png) | ![SOLAR](ScreenShots/SOLAR.png) |

| FIRES | USGS | SETTINGS |
|-------|------|----------|
| ![FIRES](ScreenShots/FIRES.png) | ![USGS](ScreenShots/USGS.png) | ![SETTINGS](ScreenShots/SETTINGS.png) |

### Features

**NOW screen:**
- Current temperature with HI/LO and dynamic feels-like (heat index when hot/humid, wind chill when cold/windy)
- Temperature trend arrow (rising/falling)
- Weather icon and condition text
- UV index, humidity, wind speed with cardinal direction, barometric pressure (hPa), visibility, dew point
- Moon phase: phase name, illuminated disk graphic, illumination %, and days to full moon
- Sunrise and sunset times

**HOURLY screen:**
- 12-hour hourly forecast — hour, weather icon, temperature, wind speed

**5-DAY screen:**
- 5-day forecast — day name, weather icon, high/low temps, wind speed, condition text

**SOLAR screen:**
- Kp index with numeric value, condition text, and color-coded severity
- Solar wind speed, Bz/Bt magnetic field components, plasma density, solar wind temperature
- X-ray flux class, flare class and peak time, CME speed and detection time
- Aurora visibility label
- 24-hour Kp history bar chart

**FIRES screen:**
- NASA EONET wildfire events — up to 12 recent events with location and date

**USGS screen:**
- USGS earthquake feed — up to 12 recent M3.5+ events with magnitude, location, and time

**NEWS screen:**
- Breaking news from r/news — crime, disasters, major incidents. Strictly moderated (no fluff, no opinion).
- Two-line headline format with word-boundary wrapping for readability
- Sorted newest-first by date
- Vertical swipe scrolling — up to 16 headlines, swipe up/down to page through all entries
- Scroll indicators (^ / v) show when more content is available above or below
- Tap title bar to force a data re-fetch; scroll position resets to top on refresh
- No API key required

**SETTINGS screen:**
- WiFi status and IP, city, NTP time sync status, weather provider info
- 6-step brightness — AUTO (LDR light sensor), DIM, LOW, MED, HIGH, MAX; saved across reboots
- Screen auto-rotate interval (off / 5s / 10s / 30s / 1m)
- Refresh Weather and Update Location buttons
- 9 theme accent colors, saved across reboots
- E-Ink mode — inverts display to white-background / black-foreground, toggled from the settings screen
- Power Off — two-tap confirmation ("PWR OFF" then "SURE?") puts the device into deep sleep (wake via RST button)

**General:**
- Auto-detects location via IP geolocation; no config needed beyond WiFi credentials
- Weather data via Open-Meteo; no API key required
- Refreshes at the top of each hour
- Tap the title bar on any screen to force an immediate data re-fetch (weather screens) or data-screen refresh (SOLAR, FIRES, USGS, NEWS)
- FIRES and USGS headline text uses the active theme color
- 8 screens total: NOW, HOURLY, 5-DAY, SOLAR, FIRES, USGS, NEWS, SETTINGS

### Setup

Flash `firmware.bin` from the latest release, then provision WiFi from the SD card:

1. Create `wifi.txt` on the SD card.
2. Put your WiFi network name on line 1.
3. Put your WiFi password on line 2.
4. Insert the SD card and boot the CYD.
5. After the first successful boot, credentials are saved to NVS and the SD card can be removed.
6. Delete `wifi.txt` from the SD card.

### Build

Build from source with PlatformIO:

```bash
pio run --environment cyd_weather
```

The generated firmware is written to `.pio/build/cyd_weather/firmware.bin`.

### Credential Safety

- WiFi credentials are not committed to Git.
- The release firmware does not require an API key.
- Open-Meteo is used for weather data, so no weather service token is baked into the build.

Built by xXQuantum-SmokeXx, with development assistance from Codex and Claude Code.