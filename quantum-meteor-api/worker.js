/**
 * Quantum-Meteor API — Cloudflare Worker
 * Serves lightweight JSON for ESP32 CYD-Weather:
 *   /fireballs — scraped IMO fireball events
 *   /ews       — EWS (Apocalypse Early Warning System) summary
 *   /cme       — latest CME speed & time from NASA DONKI (~100 bytes vs 55 KB)
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

    // GET / — simple status
    return new Response(
      JSON.stringify({ service: "Quantum-Meteor API", status: "online", endpoints: ["/fireballs", "/ews", "/cme"] }),
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
// Fetches the last 4 days of CME data (~55 KB JSON array), extracts only the
// most recent CME's startTime and speed.  ~100 bytes on the wire.
async function serveCme() {
  try {
    // Compute date range: last 4 days
    const now = new Date();
    const end = now.toISOString().split("T")[0];
    const start = new Date(now.getTime() - 4 * 86400000).toISOString().split("T")[0];

    const url = `https://api.nasa.gov/DONKI/CME?startDate=${start}&endDate=${end}&api_key=DEMO_KEY`;
    const resp = await fetch(url, {
      headers: { "User-Agent": "Quantum-Meteor/1.0", "Accept-Encoding": "identity" }
    });
    if (!resp.ok) throw new Error(`DONKI returned ${resp.status}`);

    const arr = await resp.json();
    if (!Array.isArray(arr) || arr.length === 0) {
      return json(200, { startTime: null, speed: null });
    }

    const last = arr[arr.length - 1];
    let speed = null;
    const analyses = last.cmeAnalyses;
    if (Array.isArray(analyses)) {
      for (const a of analyses) {
        if (a.speed != null) { speed = a.speed; break; }
      }
    }

    return json(200, {
      startTime: last.startTime || null,
      speed: speed,
    });
  } catch (e) {
    return json(500, { error: e.message });
  }
}

async function serveFireballs(url) {
  try {
    const limit = parseInt(url.searchParams.get("limit")) || 50;
    const html = await fetchIMO();
    const events = parseFireballs(html, limit);
    return json(200, { count: events.length, events });
  } catch (e) {
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

function json(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
    },
  });
}
