# TODO & Future Ideas

## Worker Data Sources (Quantum-Meteor)

Ideas for new data the Cloudflare Worker can aggregate, boil down, and serve to the ESP32.

### Lightning Strike Tracker 🟢 To Research
Real-time lightning strike data for tactical weather awareness.
- Potential sources: Blitzortung (crowd-sourced, real-time), NOAA LightningCast, Vaisala (commercial)

---

### Power Grid Status 🟡 Research Done
Real-time US grid telemetry — load, generation mix, frequency.
- **Kardashev API** — free hosted, no key, all 7 US ISOs. Fuel mix, load, carbon intensity, wind/solar forecasts, generator outages, battery storage, nuclear status. One GET covers the country.
- **EIA Grid Monitor** — free API key, hourly, national. Demand, generation by fuel type, interchange, CO2 emissions. ~1hr lag.
- **MISO Public API** — `public-api.misoenergy.org`, 37 anonymous JSON endpoints, 30-second resolution. Covers central US. Real-time fuel mix, load, wind/solar, generator outages.
- ESP32 payload concept: `GRID: 482GW | 38% Gas | 22% Nuclear | 14% Wind | 5% Solar | 60.00 Hz`

### Power Outage Tracking 🟡 Research Done
State-by-state customers without power, updated every 10-15 min.
- **PowerOutage.us** — gold standard (97% US, ~1000 utilities, 10-min refresh) but commercial/enterprise only. Behind Cloudflare anti-bot.
- **Outage Pro** — same data, same coverage. Commercial only, no free tier.
- **EAGLE Eye (DOE/ORNL)** — open-source platform scraping 450 utilities every 15 min. Internal DOE tool, no public API. Data published as annual bulk files (EAGLE-I dataset). ⚡ **User investigating** — if DOE exposes a public endpoint, this is the instant solution.
- **California OES ArcGIS** — ✅ **PROVEN WORKING** (2026-08-04): live, free, 15-min refresh. PG&E, SCE, SDG&E, SMUD. Fields: UtilityCompany, County, ImpactedCustomers, Cause, OutageType, EstimatedRestoreDate. Endpoint at `services.arcgis.com/BLN4oKB0N1YSgvY8/arcgis/rest/services/Power_Outages_(View)/FeatureServer/0/query`
- **GateHouseMedia/power-outages** (GitHub) — MIT-licensed Python scrapers for FPL, Duke Energy (6 states). Last updated 2020, likely broken but good reference architecture.
- Approach: Worker aggregates 5-10 state/utility ArcGIS REST endpoints → boils down to per-state outage counts + totals. No single free nationwide API exists; must be built.

### Lightning Strikes 🟢 To Research
Real-time lightning strike data for tactical weather awareness.
- Potential sources: Blitzortung (crowd-sourced, real-time), NOAA LightningCast, Vaisala (commercial)

### ISS Passes 🟢 To Research
Next visible ISS overhead pass for the user's GPS location.
- Orbital math is straightforward; several free APIs exist (Open Notify, etc.)

### Starlink Trains 🟢 To Research
Visible Starlink satellite train predictions.
- Satellite tracking APIs or TLE-based orbital prediction

---

## On-Device Features

### Scanner Removal (from memory)
WiFi scanner is decorative and interferes with data fetches.

### Battery Monitor Accuracy (from memory)
ADC noise, load sag, no smoothing. EMA filter + more samples needed.

---

## Research Notes

### EAGLE Eye / EAGLE-I
- DOE Office of Cybersecurity, Energy Security, and Emergency Response (CESER)
- Computing at Oak Ridge National Laboratory
- Open source: Docker, Java, Spring Boot, Apache Airflow
- Scrapes 450 utility outage maps every 15 minutes
- Covers ~93% of US
- EAGLE-I dataset DOI: `10.13139/ORNLNCCS/1975202`
- Presented at FOSS4GNA 2023 by Aaron Myers (GE Informatics Engineering Group)
- Status: internal DOE tool — investigating public API availability

### PowerOutage.us Architecture (for reference)
- ~10,000 lines of C# ("PowerOutageBot") on Azure
- Polls 800+ utility OMS APIs every 10 minutes
- Each utility integration manually maintained
- Output: JSON (utility, state, county, FIPS, customers tracked, customers out, timestamp) + GeoJSON shapes
- This is the model we'd replicate on a smaller scale with the Worker

---

## Completed
- [x] CME via DONKI → Quantum-Meteor Worker proxy (cached, 1hr)
- [x] EWS (Apocalypse Early Warning System) via Worker
- [x] Fireball/meteor events (IMO) via Worker
- [x] All NOAA SWPC endpoints (Kp, plasma, mag, x-ray, flare)
- [x] NWS weather alerts
- [x] WFIGS wildfire data
- [x] USGS earthquake data
- [x] USGS volcano data
- [x] r/news headlines
- [x] Open-Meteo weather (current, hourly, daily)
- [x] NWS API fallback → NWS public CDN fallback
- [x] Air quality (Open-Meteo AQI)
- [x] Solar almanac (calculated from GPS)
