/**
 * Quantum-Meteor API — Cloudflare Worker
 * Scrapes IMO fireball page, returns clean JSON for ESP32 CYD-Weather.
 * Deploy: npx wrangler deploy
 * Endpoint: https://quantum-meteor.<your-subdomain>.workers.dev/fireballs
 */

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // GET /fireballs — latest fireball events
    if (url.pathname === "/fireballs") {
      return await serveFireballs(url);
    }

    // GET / — simple status
    return new Response(
      JSON.stringify({ service: "Quantum-Meteor API", status: "online", endpoints: ["/fireballs"] }),
      { headers: { "Content-Type": "application/json" } }
    );
  }
};

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
