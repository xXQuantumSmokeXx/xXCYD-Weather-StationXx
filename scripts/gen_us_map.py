#!/usr/bin/env python3
"""Generate the CYD-Weather CONUS basemap.

Reads two Natural Earth 110m layers (public domain):

  * admin-1 boundary LINES  — each internal state border stored once, which
    avoids the double-drawn, independently-simplified shared edges that made
    the Northeast look mangled when state polygons were used.
  * admin-0 country outline — the coastline and the Canada/Mexico border.
  * 110m lakes              — the Great Lakes shorelines (Michigan needs them).

and writes:

  src/ui/us_map_data.h          int16 lon/lat polylines in PROGMEM
  scripts/us_map_preview.png    projected preview for eyeballing

Both files are build artefacts of this script — run it from anywhere:

    python scripts/gen_us_map.py

The GeoJSON is cached under scripts/.cache/ so reruns do not need the network.
"""

import json
import math
import os
import struct
import sys
import urllib.request
import zlib

# ── Must match src/ui/screens/screen_map.cpp ──────────────────────────────
CANVAS_W, CANVAS_H = 320, 170           # map owns the full content width
MAP_X0, MAP_Y0 = 0, 0
MAP_W, MAP_H = 320, 170
LON_MIN, LON_MAX = -125.0, -66.0
LAT_MIN, LAT_MAX = 24.5, 49.5
MIDLAT = 37.0
COS_MID = math.cos(math.radians(MIDLAT))

# Douglas-Peucker tolerance in degrees (projected space). 0.04° ≈ 0.27 px.
DP_TOL = 0.04

BASE_URL = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
            "master/geojson/")
URL_LINES = BASE_URL + "ne_110m_admin_1_states_provinces_lines.geojson"
URL_COUNTRY = BASE_URL + "ne_110m_admin_0_countries.geojson"
URL_LAKES = BASE_URL + "ne_110m_lakes.geojson"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "scripts", ".cache")
CACHE_LINES = os.path.join(CACHE, "ne_110m_admin_1_states_provinces_lines.geojson")
CACHE_COUNTRY = os.path.join(CACHE, "ne_110m_admin_0_countries.geojson")
CACHE_LAKES = os.path.join(CACHE, "ne_110m_lakes.geojson")
HEADER = os.path.join(ROOT, "src", "ui", "us_map_data.h")
PREVIEW = os.path.join(ROOT, "scripts", "us_map_preview.png")


def load_geojson(url, path):
    os.makedirs(CACHE, exist_ok=True)
    if not os.path.exists(path):
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def project(lon, lat):
    # The cos(lat) factor is baked into the W/H aspect, not the x scale.
    x = MAP_X0 + (lon - LON_MIN) * (MAP_W / (LON_MAX - LON_MIN))
    y = MAP_Y0 + (LAT_MAX - lat) * (MAP_H / (LAT_MAX - LAT_MIN))
    return x, y


def dp(points, tol):
    """Douglas-Peucker on [(x, y), ...] in projected space."""
    if len(points) < 3:
        return points
    # Iterative to avoid recursion limits on long coastlines.
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        den = math.hypot(dx, dy)
        best_i, best_d = -1, -1.0
        for i in range(a + 1, b):
            px, py = points[i]
            if den == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dy * px - dx * py + bx * ay - by * ax) / den
            if d > best_d:
                best_i, best_d = i, d
        if best_d > tol:
            keep[best_i] = True
            stack.append((a, best_i))
            stack.append((best_i, b))
    return [p for p, k in zip(points, keep) if k]


def simplify_coords(coords):
    # Project to lon*cos(lat) / lat so the tolerance is visual, not in degrees.
    proj = [(lon * COS_MID, lat) for lon, lat in coords]
    out = dp(proj, DP_TOL)
    # Back to lon/lat, rounded to 0.01°.
    pts = []
    for sx, lat in out:
        lon = round(sx / COS_MID, 2)
        lat = round(lat, 2)
        if not pts or pts[-1] != (lon, lat):
            pts.append((lon, lat))
    return pts


def in_conus(xs, ys, pad=0.5):
    return (min(xs) >= LON_MIN - pad and max(xs) <= LON_MAX + pad and
            min(ys) >= LAT_MIN - pad and max(ys) <= LAT_MAX + pad)


def collect():
    lines = []

    # Internal state borders. Each border is a standalone LineString, so it is
    # drawn once and cannot diverge from its neighbour.
    d = load_geojson(URL_LINES, CACHE_LINES)
    for feat in d["features"]:
        if feat["properties"].get("ADM0_A3") != "USA":
            continue
        geom = feat.get("geometry") or {}
        if geom.get("type") == "LineString":
            parts = [geom["coordinates"]]
        elif geom.get("type") == "MultiLineString":
            parts = geom["coordinates"]
        else:
            continue
        for part in parts:
            if len(part) < 2:
                continue
            xs = [p[0] for p in part]
            ys = [p[1] for p in part]
            if not in_conus(xs, ys):
                continue
            pts = simplify_coords(part)
            if len(pts) >= 2:
                lines.append(pts)

    # Coastline + Canada/Mexico border from the admin-0 country outline.
    d = load_geojson(URL_COUNTRY, CACHE_COUNTRY)
    for feat in d["features"]:
        if feat["properties"].get("ADM0_A3") != "USA":
            continue
        geom = feat.get("geometry") or {}
        if geom.get("type") == "Polygon":
            rings = geom["coordinates"]
        elif geom.get("type") == "MultiPolygon":
            rings = [r for poly in geom["coordinates"] for r in poly]
        else:
            continue
        for ring in rings:
            if len(ring) < 3:
                continue
            xs = [p[0] for p in ring]
            ys = [p[1] for p in ring]
            if not in_conus(xs, ys):
                continue
            pts = simplify_coords(ring)
            if len(pts) >= 3:
                lines.append(pts)

    # Major lakes — without the Great Lakes shorelines Michigan reads as a
    # few floating boundary segments.
    d = load_geojson(URL_LAKES, CACHE_LAKES)
    for feat in d["features"]:
        geom = feat.get("geometry") or {}
        if geom.get("type") == "Polygon":
            rings = geom["coordinates"]
        elif geom.get("type") == "MultiPolygon":
            rings = [r for poly in geom["coordinates"] for r in poly]
        else:
            continue
        for ring in rings:
            if len(ring) < 3:
                continue
            xs = [p[0] for p in ring]
            ys = [p[1] for p in ring]
            if not in_conus(xs, ys):
                continue
            pts = simplify_coords(ring)
            if len(pts) >= 3:
                lines.append(pts)

    return lines


def write_header(lines):
    values = []
    for pts in lines:
        for lon, lat in pts:
            values.append(int(round(lon * 100)))
            values.append(int(round(lat * 100)))
        values.append(-32768)   # polyline sentinel

    body = []
    for i in range(0, len(values), 12):
        body.append("    " + ", ".join(f"{v}" for v in values[i:i + 12]) + ",")

    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by scripts/gen_us_map.py — do not edit by hand.\n")
        f.write("// Source: Natural Earth 110m admin-1 boundary lines, admin-0 "
                "US outline\n// and 110m lakes (public domain).\n")
        f.write("// CONUS only; each polyline is (lon*100, lat*100), 0x8000 terminated.\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <pgmspace.h>\n\n")
        f.write("static const int16_t US_MAP_LINES[] PROGMEM = {\n")
        f.write("\n".join(body))
        f.write("\n};\n")
        f.write("#define US_MAP_LINES_LEN "
                "(sizeof(US_MAP_LINES) / sizeof(US_MAP_LINES[0]))\n")
    return len(values), len(body)


def write_preview(lines):
    scale = 3
    w, h = CANVAS_W * scale, CANVAS_H * scale
    bg = (8, 10, 14)
    fg = (0, 220, 255)
    border = (40, 60, 80)
    pix = bytearray(bg * (w * h))

    def put(x, y, c):
        if 0 <= x < w and 0 <= y < h:
            i = (y * w + x) * 3
            pix[i:i + 3] = bytes(c)

    def line(x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    for pts in lines:
        prev = None
        for lon, lat in pts:
            x, y = project(lon, lat)
            x, y = int(round(x * scale)), int(round(y * scale))
            if prev is not None:
                line(prev[0], prev[1], x, y, fg)
            prev = (x, y)

    # Outer frame so the crop is visible.
    for x in range(w):
        put(x, 0, border)
        put(x, h - 1, border)
    for y in range(h):
        put(0, y, border)
        put(w - 1, y, border)

    raw = b"".join(b"\x00" + bytes(pix[y * w * 3:(y + 1) * w * 3])
                   for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    with open(PREVIEW, "wb") as f:
        f.write(png)


def main():
    lines = collect()
    pts = sum(len(p) for p in lines)
    nvals, nlines = write_header(lines)
    write_preview(lines)
    print(f"{len(lines)} polylines, {pts} points, {nvals} int16 values "
          f"({nvals * 2} bytes)")
    print(f"wrote {HEADER}")
    print(f"wrote {PREVIEW}")


if __name__ == "__main__":
    sys.exit(main())
