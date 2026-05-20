#pragma once
#include <cstdint>

void timeSyncInit(int utcOffsetSec);
void timeSyncInitTZ(const char *tz);   // preferred — IANA string, handles DST
bool timeIsValid();
void timeGetHHMM(char *buf);          // "14:05"
void timeGetShort(char *buf);         // "2:05 PM"
void timeGetDayName(int daysAhead, char *buf3);  // "MON"
