# xXCYD-Weather-StationXx

CYD tactical monitoring station — real-time weather, space weather, solar day almanac, fireball & meteor tracking, volcano activity tracking, wildfire & earthquake tracking, breaking news, customizable display profiles, and Somnus intelligent power management — instant deep-sleep with a single tap to wake.

[![Support on Patreon](https://img.shields.io/badge/Support-Patreon-orange)](https://www.patreon.com/c/xXQuantumSmokeXx)

## Screens

| NOW | ALMANAC | SETTINGS |
|-----|---------|----------|
| ![NOW](ScreenShots/NOW.png) | ![ALMANAC](ScreenShots/ALMANAC.png) | ![SETTINGS](https://raw.githubusercontent.com/xXQuantumSmokeXx/xXCYD-Weather-StationXx/v1.2.1/ScreenShots/SETTINGS.png) |

| HOURLY | 5-DAY | SOLAR |
|--------|-------|-------|
| ![HOURLY](ScreenShots/HOURLY.png) | ![5-DAY](ScreenShots/5-DAY.png) | ![SOLAR](ScreenShots/SOLAR.png) |

| FIRES | USGS | METEORS |
|-------|------|---------|
| ![FIRES](ScreenShots/FIRES.png) | ![USGS](ScreenShots/USGS.png) | |

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
- Auto-refreshes at the top of each hour; tap title bar for manual refresh


**FIRETEAM screen:**
- 12-hour operational risk assessment for fire, storms, and heat
- LOW / ELEVATED / HIGH / CRITICAL readiness level with explainable conditions
- Actionable safety guidance for the highest forecast threat
- Official NWS alert status and severity override for the current location
- Forecast update time and highest-risk time window

**FIRES screen:**
- Dual-query WFIGS active wildfire events — fire name, state, acreage, cause, and containment %
- Primary query: active uncontained fires (newest 40). Secondary query: all fires ≥10k acres regardless of containment status (largest 30)
- Merged & deduplicated by IRWIN ID — catches large contained fires the active-only query would miss
- 30 entries, scrollable via swipe up/down (12 per page, 2.5 pages)
- Sorted newest-first; tap title bar to force a refresh
- Prescribed burns automatically filtered out; auto-refreshes hourly

**USGS screen:**
- USGS earthquake feed — M3.5+ events with magnitude, location, and time (UTC, 12-hour format)
- 36 entries, scrollable via swipe up/down (12 per page, exactly 3 pages)
- Sorted newest-first (API-native ordering); tap title bar to force a refresh
- Auto-refreshes every 15 minutes

**METEORS screen:**
- IMO (International Meteor Organization) crowd-sourced fireball events — real witness reports from today
- Clear columns: date, witness count, country:state location, and event tags (B = sonic boom, F = fragmentation/meteorites, C = concurrent/electrophonic sound)
- Themed throughout with alternating row backgrounds for readability
- Data via Quantum-Meteor API (lightweight proxy, no key required)
- Scrollable via swipe up/down; tap title bar to force refresh; auto-refreshes hourly
- 30 most recent events displayed per page

**VOLCANOES screen:**
- USGS volcano activity — Yellowstone volcano tracking plus currently elevated volcanoes
- Color-coded status dot: GREEN (normal), YELLOW (advisory), ORANGE (watch), RED (warning)
- Two-line per volcano: name on top, observatory / alert level / last-update timestamp below
- Sorted newest-first; tap title bar to force a refresh
- Auto-refreshes on the hour

**NEWS screen:**
- Breaking news from r/news — crime, disasters, major incidents. Strictly moderated (no fluff, no opinion).
- Two-line headline format with word-boundary wrapping for readability
- Sorted newest-first by date
- Vertical swipe scrolling — up to 16 headlines, swipe up/down to page through all entries
- Tap title bar to force a data re-fetch; scroll position resets to top on refresh
- No API key required

**ALMANAC screen:**
- Solar day timeline calculated from GPS coordinates — no API needed
- 9 phases: astronomical dawn, nautical dawn, civil dawn, sunrise, solar noon, sunset, civil dusk, nautical dusk, astronomical dusk
- Each phase shows calculated time, phase label, and practical field note (e.g. "Horizon visible", "Golden hour begins")
- Daylight duration hero line at top (e.g. "10h 23m of daylight")
- Falls back to OpenWeatherMap sunrise/sunset times when available

**SETTINGS screen:**
- **Sleep Timer** — backlight auto-off after inactivity (OFF / 15s / 30s / 1m / 5m). Tap or swipe to wake
- **Scheduled Sleep & Wake Timer** — daily schedule to turn the backlight off during configured sleep hours (e.g. 10PM–7AM), every day. Toggle on with SCHED, then tap SLEEP and WAKE to cycle through preset hours. ESP32 stays running so NTP time and all data fetches continue normally. Backlight off saves display life without losing connectivity. Tap anywhere during the sleep window for a 30-second wake grace period before the schedule re-enforces. Independent of the inactivity timer — both can be active simultaneously, with the schedule acting as the daily default and the quick timers as one-off overrides.
- 6-step brightness — AUTO (LDR light sensor), DIM, LOW, MED, HIGH, MAX; saved across reboots
- Screen auto-rotate interval (off / 5s / 10s / 30s / 1m)
- 11 per-page rotation checkboxes — toggle which screens are included in auto-rotate
- **Favorite screens** — double-tap any page checkbox to mark it as a favorite (amber highlight). Favorites are interspersed between regular screens during rotation (e.g. Fav, 2, Fav, 3, Fav, 4…), so your most-used screens appear twice as often. Multiple favorites cycle round-robin. Double-tap again to un-favorite; disabling a page auto-clears its favorite
- Refresh Weather and Location buttons in a 2x2 grid next to E-Ink and Power Off
- 9 theme accent colors, saved across reboots
- E-Ink mode — inverts display to white-background / black-foreground, toggled from the settings screen
- Power Off — two-tap confirmation ("PWR OFF" → "SURE?") engages **Somnus Deep-Sleep Recovery**: ESP32 enters ultra-low-power deep sleep with EXT0 touch-IRQ wake on GPIO 36. A single screen tap reanimates the device instantly — no hardware reset required

**General:**
- Auto-detects location via IP geolocation; no config needed beyond WiFi credentials
- Weather data via Open-Meteo; no API key required
- Refreshes at the top of each hour
- Tap the title bar on any screen to force an immediate data re-fetch (weather screens) or data-screen refresh (SOLAR, FIRES, USGS, NEWS)
- FIRES and USGS headline text uses the active theme color
- 12 screens total: NOW, HOURLY, 5-DAY, SOLAR, FIRETEAM, FIRES, USGS, METEORS, VOLCANOES, NEWS, ALMANAC, SETTINGS

### Setup

| Board | Firmware File | How to Identify |
|-------|--------------|-----------------|
| **ESP32-32E** (1-USB) | `CYD-Weather-1usb.bin` | Single USB port, "ESP32-32E" on chip |
| **2-USB** (2 USB ports) | `CYD-Weather-2usb.bin` | Two USB ports, any chip marking |

These are **merged flash images** — bootloader + partition table + application firmware combined into a single file. Flash at offset **0x00** with any ESP32 tool (esptool, ESP32 Flash Download Tool, BinForge, etc.). No need to hunt down separate bootloader or partition files.

Flash the correct firmware for your board from the latest release, then provision WiFi from the SD card:

1. Create `wifi.txt` on the SD card.
2. Put your WiFi network name on line 1.
3. Put your WiFi password on line 2.
4. Insert the SD card and boot the CYD.
5. After the first successful boot, credentials are saved to NVS and the SD card can be removed.
6. Delete `wifi.txt` from the SD card.

### Build

Build from source with PlatformIO:

```bash
# ESP32-32E (1-USB)
pio run --environment cyd_weather

# 2-USB
pio run --environment cyd_weather_2usb
```

After a successful build, the post-build script `merge_bin.py` automatically merges bootloader + partition table + application firmware into a single flashable image at the project root:

| Environment | Source Name | Release Name |
|-------------|-------------|-------------|
| `cyd_weather` | `merged-firmware-cyd_weather.bin` | `CYD-Weather-1usb.bin` |
| `cyd_weather_2usb` | `merged-firmware-cyd_weather_2usb.bin` | `CYD-Weather-2usb.bin` |

Flash the merged file at offset **0x00**. No extra dependencies needed — the post-build merge uses esptool.py bundled with PlatformIO.

### Credential Safety

- WiFi credentials are not committed to Git.
- The release firmware does not require an API key.
- Open-Meteo is used for weather data, so no weather service token is baked into the build.

## First-Boot Calibration (2-USB only)

On first boot, the firmware walks through **two** calibration steps to match your board's specific LCD and digitizer orientation. All settings are saved to NVS and never repeat after calibration — use **serial `T` command** or flash a fresh device to re-trigger.

### Step 1: Display Mode (LCD orientation)

The ILI9341 display on 2-USB boards can be mounted in different physical orientations. The firmware cycles through four hardware modes — tap the screen to cycle, hold your finger for 2 seconds to confirm.

**The four modes are:**

| Mode | MADCTL | What it does | Who needs it |
|------|--------|-------------|-------------|
| **0** | `0x28` | Swaps rows & columns — 90° rotation fix | Portrait-glass panels (rare) |
| **1** | `0xA8` | Swap + Y-mirror | Portrait-glass, physically flipped |
| **2** | `0x00` | Clean landscape — no swap, no mirror | Landscape-glass panels (common) |
| **3** | `0x80` | Y-mirror only — like v1.1.7 original | Landscape-glass, physically flipped (most 2-USB) |

A large asymmetric reference pattern (triangle, L-bracket, ring, crosshair, and a center "T") shows on screen so you can see each mode's effect clearly. Pick the one where the text reads correctly and the corner markers are in the right places.

**If the touch doesn't match the display** (e.g. taps register 180° off), that's normal — touch calibration comes next. Finish display calibration first, then touch.

### Step 2: Touch (digitizer orientation)

After display mode is confirmed, a touch calibration screen appears. A cursor follows your finger — tap to cycle through the four XPT2046 digitizer rotations until the cursor tracks your finger without offset. Four crosshair targets in the corners let you verify alignment. Hold 2 seconds to confirm.

You can re-run touch calibration anytime from **serial `T` command**.

### Serial Commands

Connect at 115200 baud. Single-character commands, no newline required:

| Command | Effect |
|---------|--------|
| `R` | Responds with `READY` — useful for serial monitor sync |
| `M` | Cycle display mode 0→1→2→3→0. Fixes wrong LCD orientation without reflashing |
| `T` | Cycle touch digitizer rotation 0→1→2→3. Fixes offset touch |
| `0`–`9` | Jump directly to screen 0–9 |
| `A` | Jump to Settings screen |
| `S` | RGB332 screenshot capture (for `screenshot.py`) |

## 2-USB Variant Notes

The "2-USB" label covers several hardware revisions. Known differences:

- **Display glass orientation**: Some 2-USB boards mount the ILI9341 in landscape (modes 2–3), others in portrait (modes 0–1). The first-boot calibration handles all four.
- **Touch controller**: All known 2-USB variants use XPT2046 (same as 1-USB), but digitizer rotation varies — the first-boot touch calibration handles this.
- **TFT_RST pin**: Most 2-USB boards work with RST unconnected (the ILI9341 self-resets on power-on). A subset need RST connected to GPIO 4. If you get a **solid black screen with a faint red LED** on power-up, try the 2-USB binary — if that also fails, compile from source with `-DTFT_RST=4` in `platformio.ini` build_flags.
- **SPI frequency**: The default 27 MHz works on all tested boards. If you see display corruption (glitchy pixels, wrong colors), try dropping to 20 MHz by adding `-DSPI_FREQUENCY=20000000` to build_flags.

### Troubleshooting Checklist

**Screen stays black after flashing:**
1. **Verify the right binary** — `CYD-Weather-2usb.bin` for 2-USB boards, `CYD-Weather-1usb.bin` for 1-USB
2. **Offset 0x00** — flash the merged binary at offset 0, not the raw firmware.bin
3. **Try TFT_RST=4** — some 2-USB boards need GPIO4 as display reset. Compile with `-DTFT_RST=4`
4. **First-boot calibration** — if the screen lights up but looks wrong, the on-screen calibration should find the right mode. If it doesn't appear, connect via serial and type `M` to cycle modes, `T` to cycle touch rotation

**Display is rotated 90°:**
- Cycle through modes 0–3 via serial `M`. One of them will be correct.
- If none work, your board may need a different TFT_RST or SPI frequency — see notes above.

**Touch is offset or 180° off:**
- Type `T` over serial to cycle through touch rotations.
- Or re-run full calibration from **serial `T` command**.

Check out my other project: [xXCYD-PokerXx](https://github.com/xXQuantumSmokeXx/xXCYD-PokerXx)

Built by xXQuantum-SmokeXx, with development assistance from Codex & Claude Code.

UI design ported from the "QuantumSix" "VR project", originally developed in collaboration with "Six" & "Nova"...


