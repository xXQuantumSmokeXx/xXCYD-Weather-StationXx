# TODO — CYD-Weather

> Last updated: 2026-08-04

---

## 🚀 Future Ideas (someday / research phase)

Ideas for new data the Quantum-Meteor Worker can aggregate, boil down, and serve to the ESP32.

### Lightning Strike Tracker 🟡 Research Done
Real-time lightning strike data for tactical weather awareness.
- **Blitzortung** — worldwide community network, ~100 strikes/min globally, free for non-commercial use with attribution. No official REST API.
- **Data flow:** WebSocket (`wss://ws1.blitzortung.org/`) → LZW decompress (starts code 256) → one JSON strike per frame → normalize. Handshake: `{"a": 111}` (value rotates, scrape from `map.blitzortung.org` JS bundle if stale). Keepalive: 30s ping, reconnect w/ exponential backoff (1s→30s cap).
- **Strike shape:** `{"lat": 34.05, "lon": -118.24, "time": 1785859200000, "pol": 1, "region": 0, "e": 0.42, "mode": "live"}` — `e` is relative intensity proxy (0.15–1.0), not calibrated kA.
- **Mirrors:** ws1, ws2, ws7, ws8 — round-robin on failure.
- **Worker approach:** Durable Object maintains persistent WebSocket → rolling 30-min strike buffer → query by lat/lon for closest strikes. Reference: `fltman/lightings` (GitHub) Node relay with LZW decoder.
- **ESP32 payload concept:** `{"strikes":47,"closest":3.2,"direction":"SW","minutes":5}`
- **Alternative:** Xweather (Vaisala) REST API — 15K free calls/mo, includes strike type/intensity. Not Blitzortung-sourced.

### Power Grid Status 🟡 Research Done
Real-time US grid telemetry — load, generation mix, frequency.
- **Kardashev API** — free hosted, no key, all 7 US ISOs. Fuel mix, load, carbon intensity, wind/solar forecasts, generator outages, battery storage, nuclear status.
- **EIA Grid Monitor** — free API key, hourly, national. Demand, generation by fuel type, interchange, CO2 emissions. ~1hr lag.
- **MISO Public API** — `public-api.misoenergy.org`, 37 anonymous JSON endpoints, 30-second resolution. Covers central US.
- ESP32 payload concept: `GRID: 482GW | 38% Gas | 22% Nuclear | 14% Wind | 5% Solar | 60.00 Hz`

### Power Outage Tracking 🟡 Research Done
State-by-state customers without power, updated every 10-15 min.
- **PowerOutage.us** — gold standard (97% US, ~1000 utilities, 10-min refresh) but commercial/enterprise only.
- **Outage Pro** — same data, same coverage. Commercial only, no free tier.
- **EAGLE Eye (DOE/ORNL)** — open-source platform scraping 450 utilities every 15 min. Internal DOE tool, no public API. ⚡ **User investigating** — if DOE exposes a public endpoint, this is the instant solution.
- **California OES ArcGIS** — ✅ PROVEN WORKING (2026-08-04): live, free, 15-min refresh. PG&E, SCE, SDG&E, SMUD.
- Approach: Worker aggregates 5-10 state/utility ArcGIS REST endpoints → boils down to per-state outage counts + totals. No single free nationwide API exists; must be built.

### ISS Passes 🟢 To Research
Next visible ISS overhead pass for the user's GPS location.

### Starlink Trains 🟢 To Research
Visible Starlink satellite train predictions.

### Internet Outage Monitor 🟢 To Research
Internet connectivity status — is your connection up, or is it the wider net?
- Potential sources: Cloudflare Radar, ThousandEyes, DownDetector, BGP route monitoring
- Tactical value: "is it just me, or is the internet down?" — answers whether local network, ISP, or regional infrastructure is affected

### GPS / GNSS Status 🟢 To Research
GPS accuracy and satellite constellation health for your location.
- WAAS/DGPS correction status, satellite count overhead, dilution of precision
- Tactical value: knowing whether GPS is degraded before relying on it

### Cell Tower Status 🟢 To Research
Nearby cell tower proximity and carrier status.
- Potential sources: OpenCelliD, MLS (unlicensed), carrier-specific APIs
- Tactical value: comms readiness, signal strength context

### NWS Watches & Warnings (Expanded) 🟢 To Research
Already pulling active alerts — expand to flood watches, tsunami warnings, red flag warnings, tornado watches, hurricane tracks.
- Tactical value: "what's about to hit me" vs "what's already happening"

### Disaster Declarations 🟢 To Research
FEMA disaster declarations, state of emergency, evacuation zones.
- Potential sources: FEMA API, state emergency management feeds
- Tactical value: official recognition that a situation is serious

### Aviation — NOTAMs & TFRs 🟢 To Research
Notices to Air Missions and Temporary Flight Restrictions near your location.
- Potential sources: FAA NOTAM API, SkyVector, ADS-B Exchange
- Tactical value: TFRs often precede news — VIP movement, wildfire air ops, disaster response, restricted airspace activations

### Aviation — ADS-B Traffic Density 🟢 To Research
Aircraft density anomalies — sudden silence or unusual clustering overhead.
- Potential sources: ADS-B Exchange API, OpenSky Network
- Tactical value: pattern disruption is the signal — sudden airspace clearing (ground stop) or unusual concentration

---

## 🛠 On-Device Fixes (to-do)

- **Scanner Removal** — WiFi scanner is decorative and interferes with data fetches
- **Battery Monitor Accuracy** — ADC noise, load sag, no smoothing; EMA filter + more samples needed

---

## 📋 Research Notes

### EAGLE Eye / EAGLE-I
- DOE Office of Cybersecurity, Energy Security, and Emergency Response (CESER)
- Computing at Oak Ridge National Laboratory
- Open source: Docker, Java, Spring Boot, Apache Airflow
- Scrapes 450 utility outage maps every 15 minutes, covers ~93% of US
- EAGLE-I dataset DOI: `10.13139/ORNLNCCS/1975202`
- Presented at FOSS4GNA 2023 by Aaron Myers (GE Informatics Engineering Group)

### Blitzortung LZW Decode
- WebSocket: `wss://ws1.blitzortung.org/` (mirrors: ws2, ws7, ws8)
- Handshake: `{"a": 111}` — integer rotates server-side
- Each frame: single strike, LZW-compressed string (not binary)
- Decode: code starts at 256. First char literal. Subsequent: codepoint < 256 → literal; ≥ 256 → dict lookup; KwKwK edge case → phrase = oldPhrase + first char of oldPhrase
- 30s ping, exponential backoff (1s→30s cap), next mirror each retry
- Reference: `fltman/lightings/server/blitzortung.js`

### PowerOutage.us Architecture
- ~10,000 lines of C# ("PowerOutageBot") on Azure
- Polls 800+ utility OMS APIs every 10 minutes, each manually maintained
- Output: JSON (utility, state, county, FIPS, customers tracked, customers out, timestamp) + GeoJSON shapes

---

## ✅ Completed
- [x] CME via DONKI → Worker proxy (cached, 1hr)
- [x] EWS (Apocalypse Early Warning System) via Worker
- [x] Fireball/meteor events (IMO) via Worker
- [x] NOAA SWPC endpoints (Kp, plasma, mag, x-ray, flare)
- [x] NWS weather alerts (3-tier fallback: Open-Meteo → NWS API → NWS public CDN)
- [x] WFIGS wildfire data
- [x] USGS earthquake data (M3.5+)
- [x] USGS volcano data
- [x] r/news headlines
- [x] Open-Meteo weather (current, hourly, daily)
- [x] Air quality (Open-Meteo AQI)
- [x] Solar almanac (calculated from GPS)
