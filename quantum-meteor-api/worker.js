/**
 * Quantum-Meteor API — Cloudflare Worker
 * Serves lightweight JSON for ESP32 CYD-Weather:
 *   /fireballs — scraped IMO fireball events
 *   /ews       — EWS (Apocalypse Early Warning System) summary
 *   /cme       — latest CME speed & time from NASA DONKI (~100 bytes vs 55 KB)
 *   /volcanoes — active volcanoes (USGS HANS elevated + WVAR weekly + GDACS VAAs)
 * Deploy: npx wrangler deploy
 */

// ── EWS R2 dashboard URL ──────────────────────────────────────────────────
const EWS_R2 = "https://pub-49bb6a6f314c47be9b481c25e5f6ca9e.r2.dev/dashboard.json";

export default {
  async fetch(request) {
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

    // GET / — simple status
    return new Response(
      JSON.stringify({ service: "Quantum-Meteor API", status: "online", endpoints: ["/fireballs", "/ews", "/cme", "/volcanoes"] }),
      { headers: { "Content-Type": "application/json" } }
    );
  }
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

// Primary: api.nasa.gov/DONKI/CME
async function tryDonkiPrimary() {
  try {
    const now = new Date();
    const end = now.toISOString().split("T")[0];
    const start = new Date(now.getTime() - 4 * 86400000).toISOString().split("T")[0];

    const url = `https://api.nasa.gov/DONKI/CME?startDate=${start}&endDate=${end}&api_key=DEMO_KEY`;
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

// "Thu, 13 Aug 2026 04:03:16 -0400" → "2026-08-13" (UTC date part)
function fmtDay(rfcDate) {
  const d = new Date(rfcDate);
  if (isNaN(d)) return "";
  const p = n => String(n).padStart(2, "0");
  return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())}`;
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
