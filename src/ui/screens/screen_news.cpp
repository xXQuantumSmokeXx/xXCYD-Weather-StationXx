#include "screen_news.h"
#include "../theme.h"
#include "../widgets.h"
#include "../../config/config.h"
#include "../../modules/location.h"
#include "../../modules/time_sync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#define NEWS_CACHE_MS (15UL * 60UL * 1000UL)  // 15 min refresh interval
#define NEWS_RETRY_MS (30UL * 1000UL)
#define NEWS_MAX 16
#define NEWS_ROW_H 24

// r/news — breaking US/world incidents, crime, disasters. Strictly moderated (no fluff).
#define NEWS_RSS_URL "https://www.reddit.com/r/news/.rss"

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
static char s_sync[10] = "--:--";
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
    snprintf(s_sync, sizeof(s_sync), "%s", t);
}

static void xmlDecode(char *dst, const char *src, size_t dstLen) {
    size_t di = 0;
    while (*src && di < dstLen - 1) {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0)      { dst[di++] = '&';  src += 5; continue; }
            if (strncmp(src, "&lt;", 4) == 0)       { dst[di++] = '<';  src += 4; continue; }
            if (strncmp(src, "&gt;", 4) == 0)       { dst[di++] = '>';  src += 4; continue; }
            if (strncmp(src, "&quot;", 6) == 0)     { dst[di++] = '"';  src += 6; continue; }
            if (strncmp(src, "&apos;", 6) == 0)     { dst[di++] = '\''; src += 6; continue; }
            if (strncmp(src, "&#39;", 5) == 0)      { dst[di++] = '\''; src += 5; continue; }
            dst[di++] = *src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static const char *extractTag(const char *haystack, const char *tag, char *out, size_t outLen) {
    char open[32], close[32];
    snprintf(open,  sizeof(open),  "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *start = strstr(haystack, open);
    if (!start) return nullptr;
    start += strlen(open);

    if (strncmp(start, "<![CDATA[", 9) == 0) {
        start += 9;
        const char *cend = strstr(start, "]]>");
        if (cend) {
            size_t len = cend - start;
            if (len >= outLen) len = outLen - 1;
            memcpy(out, start, len);
            out[len] = '\0';
            xmlDecode(out, out, outLen);
            return cend + 3;
        }
    }

    const char *end = strstr(start, close);
    if (!end) return nullptr;

    size_t len = end - start;
    if (len >= outLen) len = outLen - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    xmlDecode(out, out, outLen);
    return end + strlen(close);
}

static void parsePubDate(const char *pubDate, char *out, size_t outLen) {
    if (!pubDate || !pubDate[0]) { copyFit("--", out, outLen); return; }
    const char *p = pubDate;
    if (isalpha(p[0]) && isalpha(p[1]) && isalpha(p[2]) && p[3] == ',') p += 5;

    int d = 0; char mon[4] = {0};
    if (sscanf(p, "%d %3s", &d, mon) == 2 && d > 0 && d <= 31) {
        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int m = 1;
        for (; m <= 12; m++) if (strncmp(mon, months[m - 1], 3) == 0) break;
        snprintf(out, outLen, "%d/%d", m, d); return;
    }
    int y, m;
    if (sscanf(pubDate, "%d-%d-%d", &y, &m, &d) == 3 && d > 0 && d <= 31) {
        snprintf(out, outLen, "%d/%d", m, d); return;
    }
    copyFit(p, out, outLen);
}

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

static int fetchRSS(const char *url, NewsItem *items, int maxItems) {
    static WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(client, url);
    http.setTimeout(12000);
    http.addHeader("User-Agent", "CYD-Weather/1.0");
    int code = http.GET();
    if (code != 200) { http.end(); return 0; }
    String body = http.getString();
    http.end();
    if (body.length() < 100) return 0;

    int count = 0;
    const char *ptr = body.c_str();
    const char *bodyEnd = ptr + body.length();

    while (count < maxItems) {
        const char *itemStart = strstr(ptr, "<entry>");
        if (!itemStart) itemStart = strstr(ptr, "<item>");
        if (!itemStart || itemStart >= bodyEnd) break;
        bool isEntry = (itemStart[1] == 'e');
        const char *closeTag = isEntry ? "</entry>" : "</item>";
        const char *cl = strstr(itemStart, closeTag);
        if (!cl || cl >= bodyEnd) break;
        const char *itemEnd = cl + (isEntry ? 8 : 7);

        char rawTitle[256] = {};
        if (extractTag(itemStart, "title", rawTitle, sizeof(rawTitle)) && rawTitle[0]) {
            if (isMetaTitle(rawTitle)) { ptr = itemEnd; continue; }
            copyFit(rawTitle, items[count].title, sizeof(items[count].title));

            char pubDate[64];
            if (extractTag(itemStart, "updated", pubDate, sizeof(pubDate)) ||
                extractTag(itemStart, "pubDate", pubDate, sizeof(pubDate))) {
                parsePubDate(pubDate, items[count].when, sizeof(items[count].when));
            } else {
                copyFit("--", items[count].when, sizeof(items[count].when));
            }
            count++;
        }
        ptr = itemEnd;
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

    int count = fetchRSS(NEWS_RSS_URL, s_items, NEWS_MAX);
    if (count == 0) return false;

    // Sort newest-first
    qsort(s_items, count, sizeof(NewsItem), dateCmp);

    s_count = count;
    s_fetchedMs = millis();
    s_fetchedOnce = true;
    stampSync();
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
    int curY = CONTENT_Y + 4;

    tft.setTextFont(FONT_SM);

    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(MARGIN, curY);
    tft.print("BREAKING NEWS");
    curY += 14;
    tft.drawFastHLine(8, curY, SCREEN_W - 16, g_themeColor);
    curY += 3;

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
    drawBottombar(tft, dateStr, 9, 12);
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    bool doFetch = (!s_fetchedOnce || s_forceRefresh || stale()) && wifiOk && !g_newsPending;
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
    int headerH = 14 + 5;       // label + line + gap
    int listH = CONTENT_H - headerH - 4;
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
