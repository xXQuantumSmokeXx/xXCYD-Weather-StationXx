#include "screen_news.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#define NEWS_CACHE_MS (15UL * 60UL * 1000UL)  // 15 min refresh interval
#define NEWS_RETRY_MS (30UL * 1000UL)
#define NEWS_MAX 16
#define NEWS_ROW_H 24

// r/news — breaking US/world incidents, crime, disasters. Strictly moderated (no fluff).
#define NEWS_URL "http://quantum-meteor.qsmoke.workers.dev/news"

struct NewsItem {
    char title[100];
    char when[12];
};

static NewsItem s_items[NEWS_MAX];
static int s_count = 0;
static bool s_fetchedOnce = false;
static unsigned long s_fetchedMs = 0;
static unsigned long s_lastAttempt = 0;
static bool s_forceRefresh = false;
static int  s_scrollOff = 0;
static char s_sync[18] = "--:--";
bool g_newsPending = false;

static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) return;
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static String fitText(TFT_eSPI &tft, const char *src, int maxPx) {
    String out(src ? src : "");
    if (tft.textWidth(out) <= maxPx) return out;
    while (out.length() > 2) {
        out.remove(out.length() - 1);
        String c = out + "..";
        if (tft.textWidth(c) <= maxPx) return c;
    }
    return String("..");
}

static bool stale() {
    if (!s_fetchedOnce && millis() - s_lastAttempt > NEWS_RETRY_MS) return true;
    if (s_forceRefresh) return true;
    if (s_fetchedOnce) return millis() - s_fetchedMs > NEWS_CACHE_MS;
    return false;
}

static void stampSync() {
    char t[10];
    timeGetShort(t);
    snprintf(s_sync, sizeof(s_sync), "Updated %s", t);
}

// Reddit's feed carries moderator posts that aren't news.
static bool isMetaTitle(const char *title) {
    if (!title || !title[0]) return true;
    if (strstr(title, "/r/") || strstr(title, "/u/")) return true;
    if (strstr(title, "Welcome to ")) return true;
    if (strstr(title, "rule") || strstr(title, "Rule")) return true;
    if (strstr(title, "megathread") || strstr(title, "Megathread")) return true;
    if (strstr(title, "daily thread") || strstr(title, "Daily Thread")) return true;
    if (strstr(title, "monthly thread") || strstr(title, "Monthly Thread")) return true;
    if (strstr(title, "sticky") || strstr(title, "Sticky")) return true;
    if (strstr(title, "announcement") || strstr(title, "Announcement")) return true;
    if (strstr(title, "mod post") || strstr(title, "Mod Post")) return true;
    return false;
}

// The Quantum-Meteor Worker fetches the feed server-side and returns ~10
// headlines as {"items":[{"title":"...","when":"M/D"}]} (~1.1 KB).  Pulling
// Reddit's 25 KB RSS directly is what broke here: rate limits on the device's
// address, and no room for it in the screenshot build's heap.
static int fetchWorkerNews(NewsItem *items, int maxItems) {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, NEWS_URL);
    http.setTimeout(12000);
    int code = http.GET();
    if (code != 200) { http.end(); return 0; }

    StreamString ss;
    http.writeToStream(&ss);
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, ss.c_str())) return 0;
    JsonArray arr = doc["items"].as<JsonArray>();
    if (arr.isNull()) return 0;

    int count = 0;
    for (JsonObject it : arr) {
        if (count >= maxItems) break;
        const char *title = it["title"] | "";
        if (!title[0] || isMetaTitle(title)) continue;
        copyFit(title, items[count].title, sizeof(items[count].title));
        copyFit(it["when"] | "--", items[count].when, sizeof(items[count].when));
        count++;
    }
    return count;
}

// ── Parse "M/D" when string into (month, day) for sorting ──────────────
static void parseMD(const char *when, int *mo, int *dy) {
    *mo = 0; *dy = 0;
    if (!when) return;
    sscanf(when, "%d/%d", mo, dy);
}

static int dateCmp(const void *a, const void *b) {
    NewsItem *na = (NewsItem *)a, *nb = (NewsItem *)b;
    int ma, da, mb, db;
    parseMD(na->when, &ma, &da);
    parseMD(nb->when, &mb, &db);
    // Newest first: compare month then day
    if (ma != mb) return mb - ma;
    return db - da;
}

bool newsFetch(bool wifiOk, const char *city) {
    if (!wifiOk || !WiFi.isConnected()) return false;
    s_lastAttempt = millis();

    int count = fetchWorkerNews(s_items, NEWS_MAX);
    if (count == 0) return false;

    // Sort newest-first
    qsort(s_items, count, sizeof(NewsItem), dateCmp);

    s_count = count;
    s_fetchedOnce = true;
    if (count > 0) {
        s_fetchedMs = millis();
        stampSync();
    } else {
        s_fetchedMs = 0;
    }
    return true;
}

// ── Split title at word boundary for two-line display ─────────────────
static void splitTitle(TFT_eSPI &tft, const char *title, int maxPx, String &line1, String &line2) {
    if (tft.textWidth(title) <= maxPx) { line1 = title; line2 = ""; return; }

    int len = strlen(title);
    int lastSpace = -1, brk = len;
    String probe;
    probe.reserve(80);
    for (int i = 0; i < len; i++) {
        probe += title[i];
        if (title[i] == ' ') lastSpace = i;
        if (tft.textWidth(probe) > maxPx) { brk = (lastSpace > 0) ? lastSpace : i; break; }
    }
    line1 = String(title).substring(0, brk);
    line1.trim();
    const char *rest = title + brk;
    while (*rest == ' ') rest++;
    if (*rest) line2 = fitText(tft, rest, maxPx); else line2 = "";
}

// ── Draw ───────────────────────────────────────────────────────────────
static void drawNewsList(TFT_eSPI &tft) {
    const int MARGIN = 8;
    const int LINE_W = SCREEN_W - MARGIN * 2;
    const int HEADER_H = 22;

    tft.setTextFont(FONT_SM);

    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(MARGIN, CONTENT_Y + 7);
    tft.print("BREAKING NEWS");
    int sw = tft.textWidth(s_sync);
    tft.setCursor(SCREEN_W - sw - 6, CONTENT_Y + 7);
    tft.print(s_sync);
    tft.drawFastHLine(8, CONTENT_Y + HEADER_H - 4, SCREEN_W - 16, g_themeColor);

    int curY = CONTENT_Y + HEADER_H;
    int bottomY = SCREEN_H - BOTBAR_H;

    for (int i = s_scrollOff; i < s_count; i++) {
        if (curY + NEWS_ROW_H > bottomY) break;

        int dateW = tft.textWidth(s_items[i].when);
        int dateX = SCREEN_W - MARGIN - dateW;
        String l1, l2;
        splitTitle(tft, s_items[i].title, LINE_W, l1, l2);

        if (l2.length() == 0) {
            // Single line — truncate to leave room for date
            String title = fitText(tft, s_items[i].title, dateX - MARGIN - 6);
            tft.setTextColor(g_themeColor, COL_BG);
            tft.setCursor(MARGIN, curY + 2);
            tft.print(title);
            tft.setCursor(dateX, curY + 2);
            tft.print(s_items[i].when);
        } else {
            int l2w = tft.textWidth(l2);
            int l2max = LINE_W - dateW - 6;
            if (l2w > l2max) l2 = fitText(tft, l2.c_str(), l2max);
            tft.setTextColor(g_themeColor, COL_BG);
            tft.setCursor(MARGIN, curY + 1);
            tft.print(l1);
            tft.setCursor(MARGIN, curY + 13);
            tft.print(l2);
            tft.setCursor(SCREEN_W - MARGIN - dateW, curY + 13);
            tft.print(s_items[i].when);
        }
        curY += NEWS_ROW_H;
    }

}

void screenNewsDraw(TFT_eSPI &tft, bool wifiOk) {
    char timeStr[10]; timeGetShort(timeStr);
    char dateStr[28]; timeGetDateLong(dateStr, sizeof(dateStr));
    drawTopbar(tft, g_location.valid ? g_location.city : "", "NEWS", timeStr, wifiOk);
    drawBottombar(tft, dateStr, 10, 13);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (s_forceRefresh || stale()) && wifiOk && !g_newsPending;
    s_forceRefresh = false;

    if (doFetch) {
        triggerNewsFetch();
    }

    if (s_count > 0) {
        drawNewsList(tft);
    } else if (g_newsPending) {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(82, 108);
        tft.print("Fetching news...");
    } else {
        tft.setTextFont(FONT_MD);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.setCursor(wifiOk ? 58 : 92, 104);
        const char *msg = wifiOk ? "No news data" : "News offline";
        tft.print(msg);
    }
}

void screenNewsTap(TFT_eSPI &tft, int16_t x, int16_t y, bool wifiOk) {
    if (y <= TOPBAR_H && wifiOk) {
        s_forceRefresh = true;
        s_fetchedMs = 0;
        s_scrollOff = 0;
    }
}

void screenNewsSwipe(int dir) {
    // Count how many items fit on one screen
    const int HEADER_H = 22;
    int listH = CONTENT_H - HEADER_H;
    int perPage = listH / NEWS_ROW_H;

    int maxOff = s_count - perPage;
    if (maxOff < 0) maxOff = 0;

    if (dir > 0) {
        // Swipe up → scroll down one page
        s_scrollOff += perPage;
        if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    } else {
        // Swipe down → scroll up one page
        s_scrollOff -= perPage;
        if (s_scrollOff < 0) s_scrollOff = 0;
    }
}
