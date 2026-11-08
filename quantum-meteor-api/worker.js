/**
 * Quantum-Meteor API — Cloudflare Worker
 * Serves lightweight JSON for ESP32 CYD-Weather:
 *   /fireballs — scraped IMO fireball events
 *   /ews       — EWS (Apocalypse Early Warning System) summary
 *   /cme       — latest CME speed & time from NASA DONKI (~100 bytes vs 55 KB)
 *   /volcanoes — active volcanoes (USGS HANS elevated + WVAR weekly + GDACS VAAs)
 * Deploy: npx wrangler deploy
 */

import { countyCentroid, COUNTY_CENTROID_BLOB } from "./county_centroids.js";

// ── EWS R2 dashboard URL ──────────────────────────────────────────────────
const EWS_R2 = "https://pub-49bb6a6f314c47be9b481c25e5f6ca9e.r2.dev/dashboard.json";

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    // GET /fireballs — latest fireball events
    if (url.pathname === "/fireballs") {
      return await serveFireballs(url);
    }

    // GET /ews — EWS summary (no live aircraft — ~300 bytes)
    if (url.pathname === "/ews") {
      return await serveEws();
    }

    // GET /cme — latest CME from NASA DONKI (~100 bytes vs 55 KB direct)
    if (url.pathname === "/cme") {
      return await serveCme();
    }

    // GET /volcanoes — new volcanic activity worldwide
    if (url.pathname === "/volcanoes") {
      return await serveVolcanoes();
    }

    // GET /outbreaks — WHO Disease Outbreak News, normalised for the crew brains
    if (url.pathname === "/outbreaks") {
      return await serveOutbreaks(url);
    }

    // GET /grid — US power outages: national + local 24 h trends
    if (url.pathname === "/grid") {
      return await serveGrid(request, url, env);
    }

    // GET /gridmap — outage markers for the MAP, one per qualifying county
    if (url.pathname === "/gridmap") {
      return await serveGridMap(url);
    }

    // GET /storms — active tornado / severe thunderstorm / flash flood warnings
    if (url.pathname === "/storms") {
      return await serveStorms();
    }

    // GET /wxmap — CONUS rain / temperature / wind / AQI grid for MAP 2
    if (url.pathname === "/wxmap") {
      return await serveWxMap();
    }

    // GET /news — headline list (feed fetched server-side, trimmed to ~10)
    if (url.pathname === "/news") {
      return await serveNews();
    }

    // GET /solar — Kp history, solar wind, x-ray class, flares and CME in one
    if (url.pathname === "/solar") {
      return await serveSolar();
    }

    // GET /quakes — USGS list as compact JSON (global, or ?us=1 for the MAP)
    if (url.pathname === "/quakes") {
      // ?src=emsc — the EMSC catalogue as a second seismic source (2026-11-01)
      if (url.searchParams.get("src") === "emsc") return await serveEmsc(url);
      return await serveQuakes(url);
    }

    // GET /fire — merged WFIGS large + active wildfire list
    if (url.pathname === "/fire") {
      return await serveFire();
    }

    // GET /geo — the caller's location from Cloudflare's own IP data
    if (url.pathname === "/geo") {
      return await serveGeo(request);
    }

    // GET / — simple status
    return new Response(
      JSON.stringify({ service: "Quantum-Meteor API", status: "online", endpoints: ["/fireballs", "/ews", "/cme", "/volcanoes", "/outbreaks", "/grid", "/gridmap", "/storms", "/wxmap", "/news", "/solar", "/quakes", "/fire", "/geo"] }),
      { headers: { "Content-Type": "application/json" } }
    );
  },

  // Keeps /grid's 24 h series continuous while nobody is looking at the screen.
  async scheduled(event, env, ctx) {
    ctx.waitUntil((async () => {
      try {
        const { payload, allStates } = await buildGrid(env, "");
        await recordHistory(env, payload, allStates);
      } catch (e) {
        console.log(`grid cron: ${e.message}`);
      }
    })());
  },
};

// ── /ews — Apocalypse Early Warning System summary ────────────────────────
// Fetches the full dashboard.json from R2 (server-side, no ESP32 involved),
// extracts only the 7 summary fields the CYD displays.  ~400 bytes on the
// wire vs 100+ KB for the full JSON with 800+ live aircraft.
async function serveEws() {
  try {
    const resp = await fetch(EWS_R2, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" }
    });
    if (!resp.ok) throw new Error(`EWS R2 returned ${resp.status}`);

    const data = await resp.json();
    const cur = data.current || {};
    const live = data.liveStatus || {};
    const coh = data.cohort || {};

    return json(200, {
      level:   cur.emergencyLevel   ?? 1,
      alert:   cur.alertLevel       ?? "normal",
      z:       +(cur.zScore || 0).toFixed(2),
      jets:    cur.concurrentCount  ?? 0,
      baseline: Math.round(cur.baselineMean || 0),
      tracked: coh.trackedCount     ?? 0,
      updated: cur.asOf             ?? null,
      airborne: live.airborneCount  ?? 0,
    });
  } catch (e) {
    return json(500, { error: e.message });
  }
}

// ── /cme — latest Coronal Mass Ejection from NASA DONKI ───────────────────
// Dual-source with automatic fallback:
//   1. api.nasa.gov/DONKI/CME          (primary)
//   2. kauai.ccmc.gsfc.nasa.gov/DONKI  (fallback — separate NASA infra)
// Extracts only the most recent CME's startTime and speed.  ~100 bytes.
//
// Two-tier cache: primary (1 h) absorbs normal traffic (~24 calls/day,
// well under DEMO_KEY's 50/day).  Fallback (24 h) serves stale data when
// both sources are unreachable.
async function serveCme() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/cme-donki";
  const STALE_KEY = "https://qsmoke-cache/cme-donki-stale";

  // 1. Primary cache hit → return immediately, zero upstream calls
  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  // 2. Cache miss — try primary (api.nasa.gov), then fallback (CCMC)
  let result = null;
  let error = null;

  result = await tryDonkiPrimary();
  if (!result) {
    result = await tryDonkiFallback();
  }

  if (result) {
    const out = json(200, result);

    // Primary: 1 h
    out.headers.set("Cache-Control", "s-maxage=3600");
    await cache.put(CACHE_KEY, out.clone());

    // Fallback: 24 h — survives multi-source outage
    const stale = out.clone();
    stale.headers.set("Cache-Control", "s-maxage=86400");
    await cache.put(STALE_KEY, stale);

    return out;
  }

  // 3. Both sources failed — serve stale if we have it
  const stale = await cache.match(STALE_KEY);
  if (stale) return stale;

  return json(500, { error: error || "DONKI unreachable" });
}

// Primary: ccmc.gsfc.nasa.gov — DONKI LEFT api.nasa.gov in the September 2026 rewrite.
// That host now 301s every /DONKI/ path, which is why this endpoint answered
// "DONKI unreachable" before 2026-10-18. The new host serves the same JSON shape
// (startTime + cmeAnalyses[].speed) and needs no api_key.
async function tryDonkiPrimary() {
  try {
    const now = new Date();
    const end = now.toISOString().split("T")[0];
    const start = new Date(now.getTime() - 4 * 86400000).toISOString().split("T")[0];

    const url = `https://ccmc.gsfc.nasa.gov/DONKI/WS/get/CME?startDate=${start}&endDate=${end}`;
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 8000);  // 8 s timeout
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
      signal: ctrl.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) throw new Error(`DONKI returned ${resp.status}`);

    const arr = await resp.json();
    if (!Array.isArray(arr) || arr.length === 0) return null;

    const last = arr[arr.length - 1];
    const startTime = last.startTime || null;
    let speed = null;
    const analyses = last.cmeAnalyses;
    if (Array.isArray(analyses)) {
      for (const a of analyses) {
        if (a.speed != null) { speed = a.speed; break; }
      }
    }
    return { startTime, speed };
  } catch (e) {
    console.log(`DONKI primary: ${e.message}`);
    return null;
  }
}

// Fallback: CCMC DONKI CME Analysis (kauai.ccmc.gsfc.nasa.gov — separate infra)
async function tryDonkiFallback() {
  try {
    const now = new Date();
    const end = now.toISOString().split("T")[0];
    const start = new Date(now.getTime() - 4 * 86400000).toISOString().split("T")[0];

    const url = `https://kauai.ccmc.gsfc.nasa.gov/DONKI/WS/get/CMEAnalysis?startDate=${start}&endDate=${end}&mostAccurateOnly=true&api_key=DEMO_KEY`;
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 8000);  // 8 s timeout
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
      signal: ctrl.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) throw new Error(`CCMC DONKI returned ${resp.status}`);

    const arr = await resp.json();
    if (!Array.isArray(arr) || arr.length === 0) return null;

    // Filter for most-accurate entries, take the latest by time21_5
    const accurate = arr.filter(a => a.isMostAccurate);
    if (accurate.length === 0) return null;

    accurate.sort((a, b) => (b.time21_5 || "").localeCompare(a.time21_5 || ""));
    const latest = accurate[0];

    return {
      startTime: latest.time21_5 || null,
      speed: latest.speed || null,
    };
  } catch (e) {
    console.log(`DONKI fallback: ${e.message}`);
    return null;
  }
}

// ── /outbreaks — WHO Disease Outbreak News ────────────────────────────────
// Built 2026-10-18 for Nova's disease-surveillance block, after the old lane
// (four hardcoded pages with dated regexes) went stale. Nova asked for general
// outbreaks rather than one curated story, official-first, with per-item source,
// publication date, location, headline and risk language - and both the source's
// own updated time and our fetch time on the block.
//
// The WHO DON API is Sitecore; no key, no UA fuss, and it carries the fields the
// crew needs. location is not a field, so it is taken from the title suffix
// ("... - Kingdom of Saudi Arabia"), which is the shape WHO uses.
//
// Fresh cache 30 min, stale 24 h. A stale answer is marked in the body, so the
// consumer can stamp it honestly rather than passing it off as current.
const WHO_DON_API =
  "https://www.who.int/api/news/diseaseoutbreaknews" +
  "?$orderby=PublicationDateAndTime%20desc&$top={limit}&$format=json";

async function serveOutbreaks(url) {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/outbreaks-v3";
  const STALE_KEY = "https://qsmoke-cache/outbreaks-v3-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const limit = Math.min(parseInt(url.searchParams.get("limit")) || 8, 20);
    // Pull a wider window than asked for: WHO posts repeated updates to the same
    // event (five BVD/Ebola updates in the top eight on 2026-10-18), and Nova asked
    // for general coverage. Dedupe per event afterwards, newest first.
    const fetchCount = Math.min(limit * 4, 40);
    const fetchedAt = new Date().toISOString();
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 10000);
    const resp = await fetch(WHO_DON_API.replace("{limit}", fetchCount), {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
      signal: ctrl.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) throw new Error(`WHO DON returned ${resp.status}`);

    const data = await resp.json();
    const items = dedupeEvents(normaliseDon(data.value || [])).slice(0, limit);
    if (!items.length) throw new Error("No DON items parsed — upstream shape may have changed");

    const payload = {
      count: items.length,
      source: "WHO Disease Outbreak News",
      fetched_at: fetchedAt,
      source_updated: items[0].published || null,
      stale: false,
      items,
    };
    const out = json(200, payload);
    out.headers.set("Cache-Control", "s-maxage=1800");
    await cache.put(CACHE_KEY, out.clone());

    const stale = json(200, { ...payload, stale: true });
    stale.headers.set("Cache-Control", "s-maxage=86400");
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    console.log(`WHO DON: ${e.message}`);
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(502, { error: e.message, source: "WHO Disease Outbreak News", items: [] });
  }
}

function normaliseDon(rows) {
  return rows.map(it => {
    const title = String(it.Title || "").trim();
    const split = splitHeadline(title);
    return {
      id: it.DonId || null,
      title: split.headline,
      location: split.location,
      published: it.PublicationDateAndTime || null,
      risk: riskLine(it.Assessment) || riskLine(it.Summary),
      url: it.UrlName
        ? `https://www.who.int/emergencies/disease-outbreak-news/item/${it.UrlName}`
        : "https://www.who.int/emergencies/disease-outbreak-news",
      source: "WHO Disease Outbreak News",
      tier: "Official",
    };
  }).filter(it => it.title);
}

// One row per event: WHO re-posts the same outbreak with a fresh DON id and date.
// Rows arrive newest-first, so the first sighting of a title+place is the latest.
function dedupeEvents(items) {
  const seen = new Set();
  return items.filter(it => {
    const key = `${it.title}|${it.location}`.toLowerCase();
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

// WHO titles read "Disease event - Country". Greedy so the LAST dash wins:
// "Ebola disease caused by Bundibugyo virus - Democratic Republic of the Congo".
function splitHeadline(title) {
  const m = title.match(/^(.*)\s+[-–—]\s+(.+)$/);
  return m ? { headline: m[1].trim(), location: m[2].trim() }
           : { headline: title.trim(), location: "" };
}

function stripHtml(value) {
  return String(value || "")
    .replace(/<[^>]+>/g, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&(?:rsquo|lsquo|apos|#39);/g, "'")
    .replace(/&(?:ldquo|rdquo|quot|#34);/g, '"')
    .replace(/&(?:mdash|ndash);/g, "-")
    .replace(/&#?\w+;/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

// Nova asked for risk language. WHO's Assessment field is prose, and its first
// sentence is often a definition ("MERS is a zoonotic viral respiratory disease...")
// rather than a level. Prefer a sentence that names a level; fall back to the first.
function riskLine(value, max = 240) {
  const text = stripHtml(value);
  if (!text) return "";
  const sentences = text.split(/(?<=[.!?])\s+/);
  const level = /\b(very low|very high|low|moderate|high|elevated|risk of)\b/i;
  const out = (sentences.find(s => level.test(s)) || sentences[0]).trim();
  return out.length > max ? out.slice(0, max - 1).trimEnd() + "…" : out;
}

async function serveFireballs(url) {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/fireballs";
  const STALE_KEY = "https://qsmoke-cache/fireballs-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const limit = parseInt(url.searchParams.get("limit")) || 50;
    const html = await fetchIMO();
    const events = parseFireballs(html, limit);
    // Empty parse usually means the page structure changed — throw so the
    // cache isn't poisoned and stale data is served instead.
    if (!events.length) throw new Error("No events parsed — upstream structure may have changed");

    const out = json(200, { count: events.length, events });
    out.headers.set("Cache-Control", "s-maxage=900");       // 15 min — matches device cache window
    await cache.put(CACHE_KEY, out.clone());

    const stale = out.clone();
    stale.headers.set("Cache-Control", "s-maxage=21600");   // 6 h
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(500, { error: e.message });
  }
}

async function fetchIMO() {
  const resp = await fetch(
    "https://fireball.imo.net/members/imo_view/browse_events",
    { headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" } }
  );
  if (!resp.ok) throw new Error(`IMO returned ${resp.status}`);
  return await resp.text();
}

function parseFireballs(html, limit) {
  const tbody = html.match(/<tbody>(.*?)<\/tbody>/s);
  if (!tbody) throw new Error("No table found");

  const rows = [...tbody[1].matchAll(/<tr>(.*?)<\/tr>/gs)];
  const events = [];

  for (const row of rows) {
    if (events.length >= limit) break;

    // Extract <td> contents
    const tds = [...row[1].matchAll(/<td[^>]*>(.*?)<\/td>/gs)];
    if (tds.length < 6) continue;

    const cells = tds.map(td =>
      td[1]
        .replace(/<[^>]+>/g, " ")      // strip nested tags
        .replace(/&nbsp;/g, " ")       // decode &nbsp;
        .replace(/&amp;/g, "&")        // decode &amp;
        .replace(/\s+/g, " ")          // collapse whitespace
        .trim()
    );

    // cells[0]=event, [1]=reports, [2]=UT date, [3]=local, [4]=country, [5]=state,
    // cells[6]=D.Sound, [7]=C.Sound, [8]=Frag, [9]=trajectory (optional)
    const evtMatch = cells[0]?.match(/Event (\d+)-(\d+)/);
    if (!evtMatch) continue; // skip month headers

    // Parse sound/frag — "2 yes 5 no" or "7 no"
    const hasYes = (s) => /\byes\b/i.test(s || "");

    events.push({
      id: evtMatch[1],
      year: evtMatch[2],
      reports: parseInt(cells[1]) || 0,
      date_utc: (cells[2] || "").substring(0, 10),
      country: cells[4] || "",
      state: cells[5]?.split(",")[0]?.trim() || "",
      d_sound: hasYes(cells[6]),
      c_sound: hasYes(cells[7]),
      frag: hasYes(cells[8]),
    });
  }

  return events;
}

// ── /volcanoes — active volcanoes worldwide ────────────────────────────────
// Three sources, merged:
//   1. USGS HANS getElevatedVolcanoes (near-real-time): every US volcano
//      currently above NORMAL with its color code / alert level.
//   2. Smithsonian/USGS Weekly Volcanic Activity Report RSS (weekly, curated):
//      keeps only "New Eruptive Activity" / "New Unrest" items.
//   3. GDACS VO feed (Volcanic Ash Advisories, near-real-time): items first
//      added within the last 7 days — catches eruptions between Thursdays.
// Volcanic region joined from Smithsonian GVP GeoServer WFS (static data,
// cached 30 days per volcano). 6 h primary + 24 h stale cache, like /cme.
async function serveVolcanoes() {
  const cache = caches.default;
  // v2 — keys bumped when the USGS source was added so the old cached
  // empty list (both sources down) can't be served after redeploy.
  const CACHE_KEY = "https://qsmoke-cache/volcanoes-v2";
  const STALE_KEY = "https://qsmoke-cache/volcanoes-v2-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const out = json(200, await buildVolcanoes());
    out.headers.set("Cache-Control", "s-maxage=21600");       // 6 h
    await cache.put(CACHE_KEY, out.clone());

    const stale = out.clone();
    stale.headers.set("Cache-Control", "s-maxage=86400");     // 24 h
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(500, { error: e.message });
  }
}

async function buildVolcanoes() {
  const [wvarXml, gdacsXml, usgsJson] = await Promise.all([
    fetchUpstream("https://volcano.si.edu/news/WeeklyVolcanoRSS.xml", "iso-8859-1"),
    fetchUpstream("https://www.gdacs.org/xml/rss_7d.xml"),
    fetchUpstream("https://volcanoes.usgs.gov/hans-public/api/volcano/getElevatedVolcanoes"),
  ]);
  // All sources down → throw so the caller serves stale instead of caching
  // an empty list. One or two sources down is fine (partial data beats nothing).
  if (!wvarXml && !gdacsXml && !usgsJson) throw new Error("all volcano sources unreachable");

  let wvar = [];
  let gdacs = [];
  let usgs = [];
  if (wvarXml) wvar = parseWvar(wvarXml);
  if (gdacsXml) gdacs = parseGdacs(gdacsXml);
  if (usgsJson) usgs = parseUsgs(usgsJson);

  // Dedupe GDACS against WVAR (lowercase name); WVAR data is richer.
  // USGS elevated dedupes against both — the "New" entries keep their tags.
  const wvarNames = new Set(wvar.map(v => v.name.toLowerCase()));
  const fresh = gdacs.filter(g => !wvarNames.has(g.name.toLowerCase()));
  const baseNames = new Set([...wvar, ...fresh].map(v => v.name.toLowerCase()));
  const elevated = usgs.filter(u => !baseNames.has(u.name.toLowerCase()));

  // Region join via GVP WFS (cached per volcano)
  const items = await Promise.all(
    [...wvar, ...fresh, ...elevated].map(async v => ({ ...v, region: await gvpRegion(v) }))
  );

  // Sort: severity first (RED > ORANGE > YELLOW), then same-day VAAs,
  // then newest report first within groups.
  const sev = c => ({ RED: 0, ORANGE: 1, YELLOW: 2 }[c] ?? 3);
  items.sort((a, b) => sev(a.code) - sev(b.code) || (b.fresh - a.fresh) || (b.when || "").localeCompare(a.when || ""));

  const out = items.slice(0, 12).map(v => ({
    name: v.name,
    country: v.country,
    region: v.region,
    code: v.code,
    fresh: v.fresh,
    when: v.when,
  }));

  const newest = items.map(v => v.when || "").sort().pop() || null;
  return { updated: newest, count: out.length, volcanoes: out };
}

// Fetch a remote document with UA + timeout; decode ISO-8859-1 where needed.
async function fetchUpstream(url, encoding) {
  try {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 8000);
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
      signal: ctrl.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) throw new Error(`${url} returned ${resp.status}`);
    if (encoding === "iso-8859-1") {
      return new TextDecoder(encoding).decode(await resp.arrayBuffer());
    }
    return await resp.text();
  } catch (e) {
    console.log(`upstream ${url}: ${e.message}`);
    return null;
  }
}

// WVAR RSS → [{name, country, cat, vnum, when, code, fresh}]
// Title format: "Esan (Japan) - Report for 6 August-12 August 2026 - New Unrest"
function parseWvar(xml) {
  const out = [];
  for (const m of xml.matchAll(/<item>([\s\S]*?)<\/item>/g)) {
    const block = m[1];
    const t = block.match(/<title>([^<]*)<\/title>/);
    if (!t) continue;
    const tm = t[1].match(/^([^(]+)\s*\(([^)]+)\)\s*-\s*Report for\s+.*\s*-\s*(.+)$/);
    if (!tm || !tm[3].startsWith("New ")) continue;

    const guid = block.match(/#vn_(\d+)/);
    const pd = block.match(/<pubDate>([^<]*)<\/pubDate>/);

    out.push({
      name: tm[1].trim(),
      country: tm[2].trim(),
      cat: tm[3],
      vnum: guid ? guid[1] : null,
      when: pd ? fmtDay(pd[1]) : "",
      code: tm[3] === "New Unrest" ? "ORANGE" : "RED",
      fresh: false,
    });
  }
  return out;
}

// GDACS 7d RSS → VO items first added within the last 7 days.
// Ongoing eruptions stay in the feed for their whole duration (datemodified
// refreshes), so dateadded — not datemodified — marks genuinely new events.
function parseGdacs(xml) {
  const cutoff = Date.now() - 7 * 86400000;
  const out = [];
  for (const m of xml.matchAll(/<item>([\s\S]*?)<\/item>/g)) {
    const block = m[1];
    if (!/<gdacs:eventtype>VO<\/gdacs:eventtype>/.test(block)) continue;

    const added = block.match(/<gdacs:dateadded>([^<]*)<\/gdacs:dateadded>/);
    if (!added || new Date(added[1]).getTime() < cutoff) continue;

    const name = block.match(/<gdacs:eventname>([^<]*)<\/gdacs:eventname>/);
    const country = block.match(/<gdacs:country>([^<]*)<\/gdacs:country>/);
    const alert = block.match(/<gdacs:alertlevel>([^<]*)<\/gdacs:alertlevel>/);
    if (!name || !name[1].trim()) continue;

    out.push({
      name: name[1].trim(),
      country: country ? country[1].trim() : "",
      cat: "VAA",
      vnum: null,
      when: fmtDay(added[1]),
      code: { Red: "RED", Orange: "RED", Yellow: "ORANGE", Green: "YELLOW" }[alert?.[1]] || "RED",
      fresh: true,
    });
  }
  return out;
}

// USGS HANS getElevatedVolcanoes → [{name, country, cat, vnum, when, code, fresh}]
// Returns only US volcanoes currently above NORMAL (WATCH/ADVISORY/WARNING)
// with the notice's color code and alert level.  vnums match GVP numbers, so
// the WFS region join works unchanged.  "country" carries "OBS LEVEL" so the
// device shows the observatory + alert level on line 2.
const OBS_ABBR = { avo: "AVO", hvo: "HVO", cvo: "CVO", yvo: "YVO", nmi: "NMI" };
function parseUsgs(text) {
  let arr;
  try { arr = JSON.parse(text); } catch { return []; }
  if (!Array.isArray(arr)) return [];

  return arr
    .map(v => ({
      name: (v.volcano_name || "").trim(),
      country: `${OBS_ABBR[v.obs_abbr] || (v.obs_abbr || "").toUpperCase()} ${v.alert_level || ""}`.trim(),
      cat: "Elevated",
      vnum: v.vnum || null,
      when: v.sent_utc || "",
      code: v.color_code || "RED",
      fresh: false,
    }))
    .filter(v => v.name);
}

// GVP WFS region lookup — by volcano number (WVAR, USGS) or by name (GDACS-only).
// Returns subregion ("Sicily Volcanic Province") or region as fallback.
async function gvpRegion(v) {
  const cache = caches.default;
  const cql = v.vnum
    ? `Volcano_Number=${v.vnum}`
    : `Volcano_Name='${v.name.replace(/'/g, "")}'`;
  const key = "https://qsmoke-cache/gvp-wfs/" + encodeURIComponent(cql);
  const hit = await cache.match(key);
  if (hit) return await hit.text();

  const url = "https://webservices.volcano.si.edu/geoserver/GVP-VOTW/ows" +
    "?service=WFS&version=1.0.0&request=GetFeature" +
    "&typeName=GVP-VOTW:Smithsonian_VOTW_Holocene_Volcanoes" +
    "&outputFormat=application/json&CQL_FILTER=" + encodeURIComponent(cql);

  let region = "";
  try {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 8000);
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
      signal: ctrl.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) throw new Error(`WFS returned ${resp.status}`);
    const data = await resp.json();
    const p = data?.features?.[0]?.properties;
    if (p) region = p.Subregion || p.Region || "";
  } catch (e) {
    console.log(`gvpRegion ${v.name}: ${e.message}`);
  }

  const out = new Response(region, { headers: { "Cache-Control": "s-maxage=2592000" } }); // 30 d
  await cache.put(key, out.clone());
  return region;
}

// ── /grid — US power outages + 24 h trends ────────────────────────────────
// Primary source: DOE/ORNL ODIN (openenergyhub.ornl.gov), keyless OpenDataSoft.
// Coverage is the participating utilities, not every US utility — it runs
// below the MassOutage aggregate (≈11 k vs ≈76 k on 2026-10-22), so the
// device labels the figure "REPORTED OUT" rather than a national total.
//
// MassOutage (massoutage.com) has better coverage and true 24 h history, but
// its keyless tier allows 5 requests / 5 minutes PER IP and Cloudflare's
// shared Worker egress IPs exhaust that budget (confirmed: the Worker gets
// 429 within one refresh). It is used only when env.MO_API_KEY is set:
//   /live/summary          national totals + per-state counts
//   /trend/data            24 h national customers-out series
//   /trend/data?stateFips  same for the caller's state
//
// Without a key, history is sampled into KV (GRID_HISTORY) once per cache
// miss (~10 min) and by a cron trigger, so the charts fill in over 24 h.
// Fresh 10 min, stale 6 h.
const MO_BASE = "https://massoutage.com/api/v1";
const ODIN_BASE =
  "https://openenergyhub.ornl.gov/api/explore/v2.1/catalog/datasets/odin-real-time-outages-county/records";

const STATE_ABBR = {
  Alabama:"AL", Alaska:"AK", Arizona:"AZ", Arkansas:"AR", California:"CA",
  Colorado:"CO", Connecticut:"CT", Delaware:"DE", "District of Columbia":"DC",
  Florida:"FL", Georgia:"GA", Hawaii:"HI", Idaho:"ID", Illinois:"IL",
  Indiana:"IN", Iowa:"IA", Kansas:"KS", Kentucky:"KY", Louisiana:"LA",
  Maine:"ME", Maryland:"MD", Massachusetts:"MA", Michigan:"MI", Minnesota:"MN",
  Mississippi:"MS", Missouri:"MO", Montana:"MT", Nebraska:"NE", Nevada:"NV",
  "New Hampshire":"NH", "New Jersey":"NJ", "New Mexico":"NM", "New York":"NY",
  "North Carolina":"NC", "North Dakota":"ND", Ohio:"OH", Oklahoma:"OK",
  Oregon:"OR", Pennsylvania:"PA", "Rhode Island":"RI", "South Carolina":"SC",
  "South Dakota":"SD", Tennessee:"TN", Texas:"TX", Utah:"UT", Vermont:"VT",
  Virginia:"VA", Washington:"WA", "West Virginia":"WV", Wisconsin:"WI",
  Wyoming:"WY",
};

async function serveGrid(request, url, env) {
  const cache = caches.default;

  const region = (url.searchParams.get("state") ||
                  (request.cf && request.cf.region) || "").trim();

  // Cache per region: the local block depends on the caller's state.
  const rk = encodeURIComponent(region.toLowerCase() || "-");
  const CACHE_KEY = `https://qsmoke-cache/grid-v2?r=${rk}`;
  const STALE_KEY = `https://qsmoke-cache/grid-v2-stale?r=${rk}`;

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const { payload, allStates } = await buildGrid(env, region);

    // A cache miss doubles as the history sampling tick.
    const hist = await recordHistory(env, payload, allStates);
    applyHistory(payload, hist);

    const out = json(200, payload);
    out.headers.set("Cache-Control", "s-maxage=600");        // 10 min
    await cache.put(CACHE_KEY, out.clone());

    const stale = json(200, { ...payload, stale: true });
    stale.headers.set("Cache-Control", "s-maxage=21600");    // 6 h
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    console.log(`grid: ${e.message}`);
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(502, { error: e.message });
  }
}

async function buildGrid(env, region) {
  if (env && env.MO_API_KEY) {
    try { return await buildMassOutage(region, env.MO_API_KEY); }
    catch (e) { console.log(`grid massoutage: ${e.message}`); }
  }
  return await buildOdin(region);
}

// Better coverage + true history — only with a MassOutage key configured.
async function buildMassOutage(region, key) {
  const summary = await moGet("/live/summary", key);
  const nat = summary.national || {};
  const states = (summary.states || [])
    .slice()
    .sort((a, b) => (b.customers_out || 0) - (a.customers_out || 0));

  const payload = {
    updated: nat.updated_at || nat.as_of || null,
    stale: !!nat.stale,
    source: "MassOutage",
    national: { now: nat.customers_out || 0 },
    incidents: nat.incident_count || 0,
    counties: nat.county_count || 0,
    states: states.slice(0, 4).map(s => ({ abbr: s.abbr, out: s.customers_out || 0 })),
    sources: (summary.coverage && summary.coverage.sources) || 0,
  };

  // Trends are best-effort: totals still render if either call fails.
  const natTrend = await moGet("/trend/data?range=24h", key)
    .catch(e => { console.log(`grid national trend: ${e.message}`); return null; });
  if (natTrend) applyTrend(payload.national, natTrend);

  // Local state: match the caller's region against the summary's state list.
  // Cloudflare supplies cf.region as a full name ("California"); a state code
  // also matches, so ?state=CA works for manual testing.
  const match = states.find(s => region && (
    String(s.name || "").toLowerCase() === region.toLowerCase() ||
    String(s.abbr || "").toLowerCase() === region.toLowerCase()));

  if (match) {
    const local = { name: match.name, abbr: match.abbr, now: match.customers_out || 0 };
    const locTrend = await moGet(`/trend/data?range=24h&stateFips=${encodeURIComponent(match.state_fips)}`, key)
      .catch(e => { console.log(`grid local trend: ${e.message}`); return null; });
    if (locTrend) applyTrend(local, locTrend);
    payload.local = local;
  }

  return {
    payload,
    allStates: states.map(s => ({ name: s.name, abbr: s.abbr, out: s.customers_out || 0 })),
  };
}

// Reduce { series:[{out}] } into the compact fields the CYD draws.
// ~90 frames at 16 min → 30 bars, max per bucket so spikes survive; the last
// bar is forced to the current value so the chart ends on the headline number.
function applyTrend(dst, trend) {
  const raw = (trend.series || []).map(p => Math.max(0, Math.round(p.out || 0)));
  if (raw.length < 2) return;

  const BUCKETS = 30;
  const pts = [];
  for (let i = 0; i < BUCKETS; i++) {
    const a = Math.floor(i * raw.length / BUCKETS);
    const b = Math.max(a + 1, Math.floor((i + 1) * raw.length / BUCKETS));
    let m = 0;
    for (let j = a; j < b; j++) if (raw[j] > m) m = raw[j];
    pts.push(m);
  }

  const now = raw[raw.length - 1];
  pts[BUCKETS - 1] = now;

  // Direction vs ~1 h ago (4 frames), 2 % dead-band.
  const ref = raw[Math.max(0, raw.length - 5)];
  let dir = 0;
  if (ref > 0 && now > ref * 1.02) dir = 1;
  else if (ref > 0 && now < ref * 0.98) dir = -1;
  else if (ref === 0 && now > 0) dir = 1;

  dst.now = now;
  dst.ago = raw[0];
  dst.peak = Math.max(...pts);
  dst.delta = now - raw[0];
  dst.dir = dir;
  dst.series = pts;
}

async function moGet(path, key) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 8000);
  try {
    const resp = await fetch(MO_BASE + path, {
      headers: {
        "User-Agent": "Quantum-Meteor/1.0",
        "Accept": "application/json",
        "Accept-Encoding": "identity",
        ...(key ? { "Authorization": `Bearer ${key}` } : {}),
      },
      signal: ctrl.signal,
    });
    if (resp.status === 429) {
      const ra = resp.headers.get("Retry-After");
      throw new Error(`MassOutage 429 (retry ${ra || "?"}s)`);
    }
    if (!resp.ok) throw new Error(`MassOutage ${resp.status}`);
    return await resp.json();
  } finally {
    clearTimeout(timer);
  }
}

// Keyless primary: DOE/ORNL ODIN, current county-level outages summed.
async function buildOdin(region) {
  const [nat, grouped] = await Promise.all([odinSum(""), odinStates()]);
  if (!nat.total && !nat.out) throw new Error("ODIN returned no records");

  const allStates = (grouped || []).map(r => ({
    name: r.state || "",
    abbr: STATE_ABBR[r.state] || String(r.state || "").slice(0, 2).toUpperCase(),
    out: Math.round(r.out || 0),
  })).sort((a, b) => b.out - a.out);

  const payload = {
    updated: new Date().toISOString(),
    stale: false,
    source: "ODIN",
    national: { now: nat.out },
    incidents: nat.total,
    counties: 0,
    states: allStates.slice(0, 4).map(s => ({ abbr: s.abbr, out: s.out })),
    sources: 0,
  };

  if (region) {
    const m = allStates.find(s => s.name &&
      (s.name.toLowerCase() === region.toLowerCase() ||
       s.abbr.toLowerCase() === region.toLowerCase()));
    if (m) payload.local = { name: m.name, abbr: m.abbr, now: m.out };
  }

  return { payload, allStates };
}

async function odinSum(where) {
  const q = "?select=" + encodeURIComponent("sum(metersaffected) as out") +
            (where ? "&where=" + encodeURIComponent(where) : "") + "&limit=1";
  const resp = await fetch(ODIN_BASE + q, {
    headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
  });
  if (!resp.ok) throw new Error(`ODIN ${resp.status}`);
  const j = await resp.json();
  const row = (j.results && j.results[0]) || {};
  return { out: Math.round(row.out || 0), total: j.total_count || 0 };
}

async function odinStates() {
  const q = "?select=" + encodeURIComponent("state, sum(metersaffected) as out") +
            "&group_by=" + encodeURIComponent("state") +
            "&order_by=" + encodeURIComponent("out desc") +
            "&limit=60";
  const resp = await fetch(ODIN_BASE + q, {
    headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
  });
  if (!resp.ok) throw new Error(`ODIN states ${resp.status}`);
  const j = await resp.json();
  return j.results || [];
}

// ── 24 h history in KV ────────────────────────────────────────────────────
// One sample per cache miss (~10 min) plus the cron trigger. 144 points/day,
// trimmed to a rolling 24 h. Missing binding = no history; the screen still
// shows current totals.
async function recordHistory(env, payload, allStates) {
  if (!env || !env.GRID_HISTORY) return null;
  const KV = env.GRID_HISTORY;
  const now = Math.floor(Date.now() / 1000);

  let arr = [];
  try { arr = (await KV.get("hist:v1", "json")) || []; }
  catch (e) { console.log(`grid hist get: ${e.message}`); }
  if (!Array.isArray(arr)) arr = [];

  const last = arr[arr.length - 1];
  // 5-minute guard: cron fires on the 10-minute mark, but the first manual
  // sample can land seconds after it, so a 600 s guard would skip every other
  // cron tick. ~144 samples/day keeps the 24 h window and stays under KV's
  // free-tier write budget (1,000/day).
  if (!last || now - last.t >= 300) {
    const s = {};
    for (const st of allStates || []) if (st.abbr) s[st.abbr] = st.out;
    arr.push({ t: now, n: payload.national.now, s });
    while (arr.length && now - arr[0].t > 86400) arr.shift();
    if (arr.length > 200) arr.splice(0, arr.length - 200);
    try { await KV.put("hist:v1", JSON.stringify(arr)); }
    catch (e) { console.log(`grid hist put: ${e.message}`); }
  }
  return arr;
}

function applyHistory(payload, hist) {
  // A keyed MassOutage build already carries the provider's real series.
  if (!Array.isArray(hist) || payload.national.series) return;

  applyVals(payload.national, hist.map(h => h.n));
  if (payload.local && payload.local.abbr) {
    applyVals(payload.local, hist.map(h => (h.s && h.s[payload.local.abbr]) || 0));
  }
}

function applyVals(dst, vals) {
  const pts = vals.map(v => Math.max(0, Math.round(v || 0)));
  if (pts.length < 4) return;

  const liveNow = dst.now;
  pts[pts.length - 1] = liveNow;

  dst.ago = pts[0];
  dst.peak = Math.max(...pts);
  dst.delta = liveNow - pts[0];

  // Direction vs ~1 h ago (6 samples at 10 min), 2 % dead-band.
  const ref = pts[Math.max(0, pts.length - 7)];
  let dir = 0;
  if (ref > 0 && liveNow > ref * 1.02) dir = 1;
  else if (ref > 0 && liveNow < ref * 0.98) dir = -1;
  else if (ref === 0 && liveNow > 0) dir = 1;
  dst.dir = dir;

  dst.series = downsample(pts, 30);
}

function downsample(vals, buckets) {
  if (vals.length <= buckets) return vals.slice();
  const out = [];
  for (let i = 0; i < buckets; i++) {
    const a = Math.floor(i * vals.length / buckets);
    const b = Math.max(a + 1, Math.floor((i + 1) * vals.length / buckets));
    let m = 0;
    for (let j = a; j < b; j++) if (vals[j] > m) m = vals[j];
    out.push(m);
  }
  out[buckets - 1] = vals[vals.length - 1];
  return out;
}

// "Thu, 13 Aug 2026 04:03:16 -0400" → "2026-08-13" (UTC date part)
function fmtDay(rfcDate) {
  const d = new Date(rfcDate);
  if (isNaN(d)) return "";
  const p = n => String(n).padStart(2, "0");
  return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())}`;
}

// ── /news — headline list for the NEWS screen ─────────────────────────────
// The CYD used to pull Reddit's /r/news RSS directly: ~25 KB, rate-limited,
// and too big for the screenshot build's heap.  Fetch it here instead and
// hand back just the titles and dates (~1.5 KB).  NPR is the fallback for
// when Reddit refuses datacenter IPs.
const NEWS_SOURCES = [
  "https://www.reddit.com/r/news/.rss",
  "https://feeds.npr.org/1001/rss.xml",
];

async function serveNews() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/news-v2";
  const STALE_KEY = "https://qsmoke-cache/news-v2-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  let items = [];
  let error = null;
  for (const src of NEWS_SOURCES) {
    try {
      const resp = await fetch(src, {
        headers: {
          "User-Agent": "Mozilla/5.0 (compatible; Quantum-Meteor/1.0)",
          "Accept-Encoding": "identity",
        },
      });
      if (!resp.ok) { error = `${src}: HTTP ${resp.status}`; continue; }
      items = parseFeedItems(await resp.text(), 10);
      if (items.length) break;
      error = `${src}: no items`;
    } catch (e) {
      error = `${src}: ${e.message}`;
    }
  }

  if (items.length) {
    const out = json(200, { items });
    out.headers.set("Cache-Control", "s-maxage=300");
    await cache.put(CACHE_KEY, out.clone());
    const stale = out.clone();
    stale.headers.set("Cache-Control", "s-maxage=3600");
    await cache.put(STALE_KEY, stale);
    return out;
  }

  const stale = await cache.match(STALE_KEY);
  if (stale) return stale;
  return json(502, { items: [], error: error || "feed unreachable" });
}

// Atom <entry> and RSS <item> both carry a <title> and one date tag.
function parseFeedItems(xml, max) {
  const out = [];
  const blocks = xml.match(/<(entry|item)\b[\s\S]*?<\/\1>/g) || [];
  for (const block of blocks) {
    if (out.length >= max) break;
    const t = block.match(/<title[^>]*>([\s\S]*?)<\/title>/);
    if (!t) continue;
    const d = block.match(/<(updated|pubDate|published)[^>]*>([\s\S]*?)<\/\1>/);
    const title = cleanXmlText(t[1]);
    if (!title) continue;
    out.push({ title, when: mdFromDate(d ? cleanXmlText(d[2]) : "") });
  }
  return out;
}

// The screen prints "M/D" next to each headline; do the parsing here so the
// ESP32 never has to understand RFC 822 or ISO timestamps.
function mdFromDate(s) {
  const t = Date.parse(s);
  if (isNaN(t)) return "--";
  const d = new Date(t);
  return `${d.getUTCMonth() + 1}/${d.getUTCDate()}`;
}

function cleanXmlText(s) {
  return String(s)
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
    .replace(/<[^>]*>/g, "")
    .replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"').replace(/&#0?39;|&apos;/g, "'")
    .replace(/\s+/g, " ")
    .trim();
}

// ── /solar — one snapshot for the SOLAR screen ────────────────────────────
// The screen made five separate HTTPS calls to services.swpc.noaa.gov, two of
// them Range reads against 2.5 MB and 1.5 MB multi-day files, plus a DONKI
// call through /cme.  Everything it draws fits in ~400 bytes here.
const SWPC = "https://services.swpc.noaa.gov";

async function fetchRange(url, range) {
  try {
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Range": range },
    });
    if (!resp.ok && resp.status !== 206) return null;
    return await resp.text();
  } catch (e) {
    console.log(`range ${url}: ${e.message}`);
    return null;
  }
}

function fluxToClass(flux) {
  if (!(flux > 0)) return "N/A";
  const letters = "ABCMX";
  const bounds = [1e-8, 1e-7, 1e-6, 1e-5, 1e-4];
  let idx = 0;
  for (let i = 4; i >= 0; i--) if (flux >= bounds[i]) { idx = i; break; }
  return letters[idx] + (flux / bounds[idx]).toFixed(1);
}

// "2026-10-23T12:00:00Z" -> "10-23 12:00" (the screen's format)
function mmddhhmm(iso) {
  if (!iso || iso.length < 16) return "";
  return `${iso.substr(5, 5)} ${iso.substr(11, 5)}`;
}

async function serveSolar() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/solar-v1";
  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  const [kpTxt, windTxt, magTxt, xrayTxt, flareTxt] = await Promise.all([
    fetchUpstream(`${SWPC}/products/noaa-planetary-k-index.json`),
    fetchRange(`${SWPC}/json/rtsw/rtsw_wind_1m.json`, "bytes=0-4095"),
    fetchRange(`${SWPC}/json/rtsw/rtsw_mag_1m.json`, "bytes=0-4095"),
    fetchUpstream(`${SWPC}/json/goes/primary/xrays-6-hour.json`),
    fetchUpstream(`${SWPC}/json/goes/primary/xray-flares-latest.json`),
  ]);

  let kp = 0, hist = [];
  try {
    const arr = JSON.parse(kpTxt || "[]");
    const vals = [];
    for (let i = arr.length - 1; i >= 0 && vals.length < 8; i--) {
      if (typeof arr[i].Kp === "number") vals.push(arr[i].Kp);
    }
    kp = vals[0] || 0;
    hist = vals.reverse();                 // oldest → newest, like the bars
  } catch (e) { console.log(`solar kp: ${e.message}`); }

  // Both rtsw feeds are newest-first, so the first object is current.
  const firstObject = (txt) => {
    if (!txt) return null;
    const a = txt.indexOf("{"), b = txt.indexOf("}");
    if (a < 0 || b < a) return null;
    try { return JSON.parse(txt.slice(a, b + 1)); } catch { return null; }
  };
  const wind = firstObject(windTxt) || {};
  const mag = firstObject(magTxt) || {};

  let xray = 0, xclass = "N/A", flare = "NONE", flareTime = "";
  try {
    const rows = JSON.parse(xrayTxt || "[]");
    for (let i = rows.length - 1; i >= 0; i--) {
      if (rows[i].energy === "0.1-0.8nm" && typeof rows[i].flux === "number") {
        xray = rows[i].flux;
        break;
      }
    }
    xclass = fluxToClass(xray);
  } catch (e) { console.log(`solar xray: ${e.message}`); }
  try {
    const flareRows = JSON.parse(flareTxt || "[]");
    if (flareRows.length) {
      flare = flareRows[0].max_class || flareRows[0].current_class || "NONE";
      flareTime = mmddhhmm(flareRows[0].max_time || flareRows[0].time_tag || "");
    }
  } catch (e) { console.log(`solar flare: ${e.message}`); }

  // CME comes from DONKI, already trimmed by the /cme path.
  let cme = 0, cmeTime = "NONE";
  let donki = await tryDonkiPrimary();
  if (!donki) donki = await tryDonkiFallback();
  if (donki) {
    cme = donki.speed > 0 ? donki.speed : 0;
    cmeTime = donki.startTime ? mmddhhmm(donki.startTime) : "NONE";
  }

  const out = json(200, {
    kp,
    hist,
    wind: wind.proton_speed || 0,
    dens: wind.proton_density || 0,
    temp: wind.proton_temperature || 0,
    bz: mag.bz_gsm ?? 0,
    bt: mag.bt ?? 0,
    xray,
    xclass,
    flare,
    flareTime,
    cme,
    cmeTime,
  });
  out.headers.set("Cache-Control", "s-maxage=300");
  await cache.put(CACHE_KEY, out.clone());
  return out;
}

// ── /quakes — USGS as compact JSON ────────────────────────────────────────
// Default: global M3.5+ (the USGS screen).  ?us=1 returns CONUS events over
// ?days=30 for the MAP page, with lat/lon on every item and a state-name /
// abbreviation filter so border slivers of Canada/Mexico don't appear.
// CSV is 7.6 KB for 36 events; GeoJSON was 28 KB.
const CONUS_STATES = [
  "Alabama", "Arizona", "Arkansas", "California", "Colorado", "Connecticut",
  "Delaware", "Florida", "Georgia", "Idaho", "Illinois", "Indiana", "Iowa",
  "Kansas", "Kentucky", "Louisiana", "Maine", "Maryland", "Massachusetts",
  "Michigan", "Minnesota", "Mississippi", "Missouri", "Montana", "Nebraska",
  "Nevada", "New Hampshire", "New Jersey", "New Mexico", "New York",
  "North Carolina", "North Dakota", "Ohio", "Oklahoma", "Oregon",
  "Pennsylvania", "Rhode Island", "South Carolina", "South Dakota",
  "Tennessee", "Texas", "Utah", "Vermont", "Virginia", "Washington",
  "West Virginia", "Wisconsin", "Wyoming", "District of Columbia",
];

// USGS place strings mix full names ("Whites City, New Mexico") and
// abbreviations ("Lake Pillsbury, CA") depending on the contributing network.
const CONUS_ABBR = [
  "AL", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "ID", "IL", "IN",
  "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT",
  "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA",
  "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY",
  "DC",
];

function isConusPlace(place) {
  return CONUS_STATES.some((s) => place.includes(`, ${s}`)) ||
         CONUS_ABBR.some((s) => place.includes(`, ${s}`));
}

async function serveQuakes(url) {
  const cache = caches.default;
  const us = url.searchParams.get("us") === "1";
  const minMag = Math.min(9, Math.max(0, parseFloat(url.searchParams.get("min")) || 3.5));
  const days = Math.min(30, Math.max(1, parseInt(url.searchParams.get("days"), 10) || 7));
  // Constants are compile-time, so the cache key only needs the parameters.
  const CACHE_KEY = us
    ? `https://qsmoke-cache/quakes-us-v1-${minMag}-${days}`
    : "https://qsmoke-cache/quakes-v2";
  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  let query;
  if (us) {
    const start = new Date(Date.now() - days * 86400000)
      .toISOString().slice(0, 19);
    query =
      "https://earthquake.usgs.gov/fdsnws/event/1/query?format=csv" +
      `&starttime=${start}&minlatitude=24.4&maxlatitude=49.6` +
      `&minlongitude=-125&maxlongitude=-66&minmagnitude=${minMag}` +
      `&limit=100&orderby=time`;
  } else {
    query =
      "https://earthquake.usgs.gov/fdsnws/event/1/query?format=csv" +
      `&minmagnitude=${minMag}&limit=36&orderby=time`;
  }

  const csv = await fetchUpstream(query);
  const items = [];
  if (csv) {
    const lines = csv.split("\n");
    for (let i = 1; i < lines.length && items.length < 60; i++) {
      const line = lines[i].trim();
      if (!line) continue;
      const f = csvFields(line);
      if (f.length < 14) continue;
      const mag = parseFloat(f[4]);
      if (isNaN(mag)) continue;
      const place = f[13];
      if (us && !isConusPlace(place)) continue;
      const lat = parseFloat(f[1]);
      const lon = parseFloat(f[2]);
      items.push({
        m: mag,
        p: place,
        w: mdFromDate(f[0]),
        la: isNaN(lat) ? null : Math.round(lat * 100) / 100,
        lo: isNaN(lon) ? null : Math.round(lon * 100) / 100,
      });
    }
  }

  if (!items.length) return json(502, { items: [], error: "usgs unreachable" });
  const out = json(200, { items });
  out.headers.set("Cache-Control", "s-maxage=300");
  await cache.put(CACHE_KEY, out.clone());
  return out;
}

// ── /quakes?src=emsc — the EMSC catalogue, a second seismic source ────────
// USGS and EMSC are independent catalogues. The crew use the pair as a cross-check: an
// event both carry is confirmed, while a magnitude disagreement or an event only one of
// them lists is worth saying aloud. Same item shape as /quakes, so a client can read
// either without knowing which answered.
async function serveEmsc(url) {
  const cache = caches.default;
  const minMag = Math.min(9, Math.max(0, parseFloat(url.searchParams.get("min")) || 4.5));
  const days = Math.min(30, Math.max(1, parseInt(url.searchParams.get("days"), 10) || 7));
  const CACHE_KEY = `https://qsmoke-cache/emsc-v1-${minMag}-${days}`;
  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  const start = new Date(Date.now() - days * 86400000).toISOString().slice(0, 19);
  const query =
    "https://www.seismicportal.eu/fdsnws/event/1/query?format=json" +
    `&minmagnitude=${minMag}&starttime=${start}&limit=60&orderby=time`;

  try {
    const resp = await fetch(query, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" }
    });
    if (!resp.ok) throw new Error(`EMSC returned ${resp.status}`);
    const data = await resp.json();
    const items = (data.features || []).map((f) => {
      const p = f.properties || {};
      const mag = parseFloat(p.mag);
      return {
        m: isNaN(mag) ? null : mag,
        p: p.flynn_region || "Unknown region",
        w: mdFromDate(p.time || ""),
        la: typeof p.lat === "number" ? Math.round(p.lat * 100) / 100 : null,
        lo: typeof p.lon === "number" ? Math.round(p.lon * 100) / 100 : null,
        t: p.time || null,
        src: "emsc",
      };
    }).filter((i) => i.m !== null);
    if (!items.length) throw new Error("no events parsed");
    const out = json(200, { items, count: items.length, source: "EMSC" });
    out.headers.set("Cache-Control", "s-maxage=300");
    await cache.put(CACHE_KEY, out.clone());
    return out;
  } catch (e) {
    console.log(`EMSC: ${e.message}`);
    return json(502, { items: [], error: e.message, source: "EMSC" });
  }
}

// Quote-aware split: `place` can contain commas ("91 km ESE of X, India").
function csvFields(line) {
  const out = [];
  let cur = "", quoted = false;
  for (const ch of line) {
    if (ch === '"') { quoted = !quoted; continue; }
    if (ch === "," && !quoted) { out.push(cur); cur = ""; continue; }
    cur += ch;
  }
  out.push(cur);
  return out;
}

// ── /fire — merged WFIGS list ─────────────────────────────────────────────
// The device used to make both ArcGIS queries, merge, dedupe and sort; it now
// receives one finished list.
const WFIGS =
  "https://services3.arcgis.com/T4QMspbfLg3qTGWY/ArcGIS/rest/services/" +
  "WFIGS_Incident_Locations_Current/FeatureServer/0/query";
// returnGeometry is only the fallback: brand-new incidents often have null
// InitialLatitude/InitialLongitude but do have a point geometry.
const WFIGS_FIELDS =
  "&outFields=IncidentName,FireDiscoveryDateTime,IncidentSize,FireCause," +
  "PercentContained,POOState,IrwinID,InitialLatitude,InitialLongitude" +
  "&returnGeometry=true&outSR=4326&f=json";

async function wfigsRows(where, orderBy, count) {
  const url =
    `${WFIGS}?where=${encodeURIComponent(where)}${WFIGS_FIELDS}` +
    `&orderByFields=${orderBy}&resultRecordCount=${count}`;
  const txt = await fetchUpstream(url);
  if (!txt) return [];
  try {
    const doc = JSON.parse(txt);
    return (doc.features || []).map((f) => {
      const a = f.attributes || {};
      const g = f.geometry || {};
      return {
        n: a.IncidentName || "Unknown Fire",
        // POOState arrives as "US-TX"; the screen's field is two letters.
        s: (a.POOState || "").replace(/^US-/, ""),
        ac: a.IncidentSize || 0,
        c: a.FireCause || "",
        pc: a.PercentContained || 0,
        t: a.FireDiscoveryDateTime || 0,
        id: a.IrwinID || "",
        la: Number.isFinite(a.InitialLatitude) ? a.InitialLatitude
            : Number.isFinite(g.y) ? g.y : null,
        lo: Number.isFinite(a.InitialLongitude) ? a.InitialLongitude
            : Number.isFinite(g.x) ? g.x : null,
      };
    });
  } catch (e) {
    console.log(`wfigs: ${e.message}`);
    return [];
  }
}

async function serveFire() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/fire-v4";
  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  const [large, active] = await Promise.all([
    wfigsRows("IncidentSize>=10000 AND IncidentTypeCategory<>'RX'", "IncidentSize+DESC", 30),
    wfigsRows("ActiveFireCandidate=1 AND IncidentTypeCategory<>'RX' AND PercentContained<100",
              "FireDiscoveryDateTime+DESC", 40),
  ]);

  const seen = new Set();
  const merged = [];
  for (const row of [...large, ...active]) {
    if (row.id && seen.has(row.id)) continue;
    if (row.id) seen.add(row.id);
    merged.push(row);
  }
  merged.sort((a, b) => b.t - a.t);

  if (!merged.length) return json(502, { items: [], error: "wfigs unreachable" });
  const out = json(200, { items: merged.slice(0, 40) });
  out.headers.set("Cache-Control", "s-maxage=600");
  await cache.put(CACHE_KEY, out.clone());
  return out;
}

// ── /gridmap — outage markers for the MAP, one per county ────────────────
// Same ODIN source as /grid, different shape: ODIN has no coordinates, so
// each county FIPS is resolved against the bundled Census centroid table.
// Only counties at or above `min` customers appear — an unremarkable day
// would otherwise paint the map with dots of five customers each.
// ?min=N overrides the threshold (default 1000); the default is not cached
// twice, the parameter is part of the key.
const GRIDMAP_MIN = 1000;
const GRIDMAP_MAX = 80;

async function serveGridMap(url) {
  const cache = caches.default;
  const min = Math.max(0, parseInt(url.searchParams.get("min"), 10) || GRIDMAP_MIN);
  const CACHE_KEY = `https://qsmoke-cache/gridmap-v1?min=${min}`;
  const STALE_KEY = `https://qsmoke-cache/gridmap-v1-stale?min=${min}`;

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const q = "?select=" + encodeURIComponent(
      "state,county,communitydescriptor,sum(metersaffected) as out") +
      "&group_by=" + encodeURIComponent("state,county,communitydescriptor") +
      "&order_by=" + encodeURIComponent("out desc") +
      "&limit=400";
    const resp = await fetch(ODIN_BASE + q, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" },
    });
    if (!resp.ok) throw new Error(`ODIN ${resp.status}`);
    const doc = await resp.json();

    let national = 0;
    const items = [];
    for (const row of doc.results || []) {
      const out = Math.round(row.out || 0);
      national += out;
      if (out < min || items.length >= GRIDMAP_MAX) continue;
      const pt = countyCentroid(row.communitydescriptor);
      if (!pt) continue;                        // territory outside the table
      items.push({
        f: String(row.communitydescriptor).padStart(5, "0"),
        la: pt[0],
        lo: pt[1],
        o: out,
      });
    }

    if (!items.length && !national) throw new Error("ODIN returned no records");
    const payload = {
      updated: new Date().toISOString(),
      src: "ODIN",
      min,
      total: national,
      items,
    };
    const out = json(200, payload);
    out.headers.set("Cache-Control", "s-maxage=600");     // 10 min, like /grid
    await cache.put(CACHE_KEY, out.clone());

    const stale = json(200, { ...payload, stale: true });
    stale.headers.set("Cache-Control", "s-maxage=21600");
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    console.log(`gridmap: ${e.message}`);
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(502, { items: [], error: e.message });
  }
}

// ── /storms — active tornado, severe thunderstorm and flash flood warnings ─
// NWS publishes no compact feed: /alerts/active returns full CAP documents
// (~1 MB with every advisory in the country).  Filtering happens upstream via
// ?event=, then each warning is reduced to one representative point:
//   • polygon centroid when the alert carries geometry (most warnings do not —
//     they are issued by zone, and 255 of 273 active alerts had none), or
//   • the centroid of the first county in its SAME geocode.
// Duplicates are common: a warning is re-issued as it is extended, and two
// adjacent offices can cover the same storm.  Same event within ~25 km, or a
// shared first SAME code, collapses to the most severe copy.
const NWS_ALERTS = "https://api.weather.gov/alerts/active";
const NWS_UA = "Quantum-Meteor/1.0 (qsmoke workers)";
const STORM_EVENTS = [
  { name: "Tornado Warning", sev: 3 },
  { name: "Severe Thunderstorm Warning", sev: 2 },
  { name: "Flash Flood Warning", sev: 1 },
];
const STORM_MAX = 48;
const STORM_DEDUPE_KM = 25;

function kmBetween(a, b) {
  const R = 6371, rad = Math.PI / 180;
  const dLat = (b[0] - a[0]) * rad, dLon = (b[1] - a[1]) * rad;
  const s = Math.sin(dLat / 2) ** 2 +
    Math.cos(a[0] * rad) * Math.cos(b[0] * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(s));
}

// Vertex average of the largest ring.  Good to a few km on a warning polygon,
// which is finer than the 320-px map can show.
function polygonPoint(geom) {
  if (!geom) return null;
  let rings = [];
  if (geom.type === "Polygon") rings = [geom.coordinates[0]];
  else if (geom.type === "MultiPolygon") rings = geom.coordinates.map((p) => p[0]);
  let best = null;
  for (const ring of rings) {
    if (!Array.isArray(ring) || ring.length < 3) continue;
    if (!best || ring.length > best.length) best = ring;
  }
  if (!best) return null;
  let la = 0, lo = 0, n = 0;
  for (const [x, y] of best) {
    if (typeof x !== "number" || typeof y !== "number") continue;
    lo += x; la += y; n++;
  }
  return n ? [la / n, lo / n] : null;
}

function alertPoint(props, geom) {
  const poly = polygonPoint(geom);
  if (poly) return poly;
  for (const same of (props.geocode && props.geocode.SAME) || []) {
    // SAME is a 6-digit code (state + 3-digit county); the table is keyed by
    // the 5-digit county GEOID, which drops the leading state digit.
    const pt = countyCentroid(String(same).padStart(6, "0").slice(-5));
    if (pt) return pt;
  }
  return null;
}

async function serveStorms() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/storms-v1";
  const STALE_KEY = "https://qsmoke-cache/storms-v1-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const events = STORM_EVENTS.map((e) => e.name).join(",");
    const resp = await fetch(
      `${NWS_ALERTS}?status=actual&message_type=alert&event=${encodeURIComponent(events)}`,
      { headers: { "User-Agent": NWS_UA, "Accept": "application/geo+json" } });
    if (!resp.ok) throw new Error(`NWS ${resp.status}`);
    const doc = await resp.json();

    const sevOf = new Map(STORM_EVENTS.map((e) => [e.name, e.sev]));
    const picked = [];
    for (const f of doc.features || []) {
      const props = f.properties || {};
      const sev = sevOf.get(props.event);
      if (!sev) continue;                       // other advisories (SAME is shared)
      const pt = alertPoint(props, f.geometry);
      if (!pt) continue;

      const same = (props.geocode && props.geocode.SAME) || [];
      const dup = picked.some((p) =>
        p.s === sev && (
          (same.length && p.same === same[0]) ||
          kmBetween([p.la, p.lo], pt) < STORM_DEDUPE_KM));
      if (dup) continue;

      picked.push({ la: pt[0], lo: pt[1], s: sev, same: same[0] || "",
                    end: props.ends || props.expires || "" });
    }

    // Most severe first so the device's declutter ring drops flood warnings
    // before tornado warnings when the map is busy.
    picked.sort((a, b) => b.s - a.s);
    const items = picked.slice(0, STORM_MAX)
      .map((p) => ({ la: Math.round(p.la * 100) / 100,
                     lo: Math.round(p.lo * 100) / 100,
                     s: p.s }));

    const payload = {
      updated: new Date().toISOString(),
      n: items.length,
      byType: [3, 2, 1].map((s) => items.filter((i) => i.s === s).length),
      items,
    };
    const out = json(200, payload);
    out.headers.set("Cache-Control", "s-maxage=120");    // warnings move fast
    await cache.put(CACHE_KEY, out.clone());

    const stale = json(200, { ...payload, stale: true });
    stale.headers.set("Cache-Control", "s-maxage=1800");
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    console.log(`storms: ${e.message}`);
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(502, { items: [], error: e.message });
  }
}

// ── /wxmap — CONUS weather field for MAP 2 ────────────────────────────────
// A 12×7 grid over the same bbox the device map uses (lon -125..-66,
// lat 24.5..49.5).  Each cell is snapped to the nearest county centroid so
// every sample lands on the CONUS and nothing is sampled out over the ocean;
// cells with no centroid within 150 km are dropped.  One payload carries
// 24 h rain (hundredths of an inch), current temperature (°F), wind (mph +
// direction) and US AQI.  Open-Meteo accepts comma-separated coordinate
// lists; 40 points per request keeps each URL short.
const WX_W = 12;
const WX_H = 7;
const WX_LON0 = -125.0;
const WX_LAT0 = 49.5;
const WX_DLON = 59.0 / WX_W;
const WX_DLAT = 25.0 / WX_H;
const WX_CHUNK = 40;
const WX_LAND_KM = 150;
const WX_FORECAST = "https://api.open-meteo.com/v1/forecast";
const WX_AIR = "https://air-quality-api.open-meteo.com/v1/air-quality";

let _wxPoints = null;

function wxPoints() {
  if (_wxPoints) return _wxPoints;

  const land = [];
  for (const row of COUNTY_CENTROID_BLOB.split(";")) {
    const a = row.indexOf(",");
    const b = row.indexOf(",", a + 1);
    if (a < 0 || b < 0) continue;
    land.push([+row.slice(a + 1, b), +row.slice(b + 1)]);   // [lat, lon]
  }

  const used = new Set();
  const pts = [];
  for (let r = 0; r < WX_H; r++) {
    for (let c = 0; c < WX_W; c++) {
      const la = WX_LAT0 - (r + 0.5) * WX_DLAT;
      const lo = WX_LON0 + (c + 0.5) * WX_DLON;
      const cosLat = Math.cos(la * Math.PI / 180);
      let best = Infinity, bi = -1;
      for (let i = 0; i < land.length; i++) {
        const dy = (land[i][0] - la) * 111;
        const dx = (land[i][1] - lo) * 111 * cosLat;
        const d = dx * dx + dy * dy;
        if (d < best) { best = d; bi = i; }
      }
      // Keep one point per cell, and only if the nearest land is close
      // enough that the cell is not really ocean.
      if (bi < 0 || best > WX_LAND_KM * WX_LAND_KM || used.has(bi)) continue;
      used.add(bi);
      pts.push({ la: land[bi][0], lo: land[bi][1] });
    }
  }

  _wxPoints = pts;
  return pts;
}

async function wxJson(url) {
  const resp = await fetch(url, { headers: { "User-Agent": "Quantum-Meteor/1.0" } });
  if (!resp.ok) throw new Error(`open-meteo ${resp.status}`);
  const doc = await resp.json();
  return Array.isArray(doc) ? doc : [doc];
}

async function serveWxMap() {
  const cache = caches.default;
  const CACHE_KEY = "https://qsmoke-cache/wxmap-v2";
  const STALE_KEY = "https://qsmoke-cache/wxmap-v2-stale";

  const fresh = await cache.match(CACHE_KEY);
  if (fresh) return fresh;

  try {
    const pts = wxPoints();
    const chunks = [];
    for (let i = 0; i < pts.length; i += WX_CHUNK) {
      chunks.push(pts.slice(i, i + WX_CHUNK));
    }

    const parts = await Promise.all(chunks.map(async (chunk) => {
      const lat = chunk.map((p) => p.la.toFixed(3)).join(",");
      const lon = chunk.map((p) => p.lo.toFixed(3)).join(",");
      const fUrl = `${WX_FORECAST}?latitude=${lat}&longitude=${lon}` +
        "&current=temperature_2m,wind_speed_10m,wind_direction_10m" +
        "&hourly=precipitation&past_hours=24&forecast_hours=1" +
        "&temperature_unit=fahrenheit&wind_speed_unit=mph" +
        "&precipitation_unit=inch&timezone=UTC";
      const aUrl = `${WX_AIR}?latitude=${lat}&longitude=${lon}` +
        "&current=us_aqi&timezone=UTC";

      const [forecast, air] = await Promise.all([wxJson(fUrl), wxJson(aUrl)]);
      const t = [], ws = [], wd = [], aq = [], r = [];
      for (let j = 0; j < chunk.length; j++) {
        const cur = (forecast[j] && forecast[j].current) || {};
        const hourly = (forecast[j] && forecast[j].hourly) || {};
        let rain = 0;
        for (const v of hourly.precipitation || []) {
          if (Number.isFinite(v)) rain += v;
        }
        const a = air[j] && air[j].current && air[j].current.us_aqi;
        t.push(Number.isFinite(cur.temperature_2m) ? Math.round(cur.temperature_2m) : -999);
        ws.push(Number.isFinite(cur.wind_speed_10m) ? Math.round(cur.wind_speed_10m) : -1);
        wd.push(Number.isFinite(cur.wind_direction_10m)
          ? Math.round(cur.wind_direction_10m) % 360 : -1);
        aq.push(Number.isFinite(a) ? Math.round(a) : -1);
        r.push(Math.max(0, Math.round(rain * 100)));
      }
      return { t, ws, wd, aq, r };
    }));

    const t = [], ws = [], wd = [], aq = [], r = [];
    for (const p of parts) {
      t.push(...p.t); ws.push(...p.ws); wd.push(...p.wd);
      aq.push(...p.aq); r.push(...p.r);
    }

    // Per-point coordinates: the cells are snapped to county centroids, so
    // the device cannot reconstruct positions from a grid.
    const la = [], lo = [];
    for (const p of pts) { la.push(p.la); lo.push(p.lo); }

    const payload = {
      updated: new Date().toISOString(),
      n: pts.length,
      la, lo, t, ws, wd, aq, r,
    };
    const out = json(200, payload);
    out.headers.set("Cache-Control", "s-maxage=600");    // 10 min
    await cache.put(CACHE_KEY, out.clone());

    const stale = json(200, { ...payload, stale: true });
    stale.headers.set("Cache-Control", "s-maxage=21600");
    await cache.put(STALE_KEY, stale);
    return out;
  } catch (e) {
    console.log(`wxmap: ${e.message}`);
    const stale = await cache.match(STALE_KEY);
    if (stale) return stale;
    return json(502, { error: e.message, n: 0, la: [], lo: [],
                       t: [], ws: [], wd: [], aq: [], r: [] });
  }
}

// ── /geo — the caller's location from Cloudflare's own IP data ────────────
async function serveGeo(request) {
  const cf = request.cf || {};
  const tz = cf.timezone || "UTC";
  let offset = 0;
  try {
    const m = new Intl.DateTimeFormat("en-US", {
      timeZone: tz,
      timeZoneName: "longOffset",
    }).format(new Date()).match(/GMT([+-])(\d{2}):(\d{2})/);
    if (m) offset = (m[1] === "-" ? -1 : 1) * (parseInt(m[2], 10) * 3600 + parseInt(m[3], 10) * 60);
  } catch (e) { /* leave 0 */ }

  // Cloudflare leaves cf fields empty for some connections (VPNs, a few
  // carriers).  The device treats lat/lon of 0 as "no fix" and keeps its
  // cached location, but a blank city still looks broken in the top bar, so
  // fall back to the region, then the country.
  return json(200, {
    lat: parseFloat(cf.latitude) || 0,
    lon: parseFloat(cf.longitude) || 0,
    city: cf.city || cf.region || cf.country || "",
    region: cf.region || "",
    tz,
    offset,
  });
}

function json(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
    },
  });
}
