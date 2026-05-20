#include "time_sync.h"
#include <Arduino.h>
#include <time.h>
#include <cstring>
#include <cstdio>

void timeSyncInit(int utcOffsetSec) {
    configTime(utcOffsetSec, 0, "pool.ntp.org", "time.cloudflare.com");
}

void timeSyncInitTZ(const char *tz) {
    // Use IANA timezone string for automatic DST handling
    configTzTime(tz, "pool.ntp.org", "time.cloudflare.com");
}

bool timeIsValid() {
    time_t now = time(nullptr);
    return now > 1700000000L;
}

static struct tm* localNow() {
    time_t t = time(nullptr);
    return localtime(&t);
}

void timeGetHHMM(char *buf) {
    struct tm *tm = localNow();
    snprintf(buf, 6, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

void timeGetShort(char *buf) {
    struct tm *tm = localNow();
    int h = tm->tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, 10, "%d:%02d%s", h, tm->tm_min, tm->tm_hour < 12 ? "am" : "pm");
}

void timeGetDayName(int daysAhead, char *buf3) {
    static const char *days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    time_t t = time(nullptr) + (time_t)daysAhead * 86400L;
    struct tm *tm = localtime(&t);
    strncpy(buf3, days[tm->tm_wday], 3);
    buf3[3] = '\0';
}

void timeGetDateLong(char *buf, size_t len) {
    static const char *wdays[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    struct tm *tm = localNow();
    snprintf(buf, len, "%s, %s %d, %d",
             wdays[tm->tm_wday],
             months[tm->tm_mon],
             tm->tm_mday,
             tm->tm_year + 1900);
}
