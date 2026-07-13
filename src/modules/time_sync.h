#pragma once
#include <cstdint>
#include <cstddef>

void timeSyncInit(int utcOffsetSec);
void timeSyncInitTZ(const char *tz);   // preferred — IANA string, handles DST
bool timeIsValid();
void timeGetHHMM(char *buf);          // "14:05"
void timeGetShort(char *buf);         // "2:05 PM"
void timeGetDayName(int daysAhead, char *buf3);  // "MON"
void timeGetDateLong(char *buf, size_t len);     // "Tue, Jul 12, 2026"
