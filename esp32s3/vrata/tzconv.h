// Minimal timezone support replacing pytz on the Pi.
//
// There is no IANA tz database on the ESP32, so each zone is described by a
// POSIX TZ rule ("CET-1CEST,M3.5.0,M10.5.0/3"). A built-in table maps common
// TZIDs (all European zones, UTC, the Microsoft names seen in Outlook and
// Exchange exports, a few others) to rules; unknown TZIDs can still be
// resolved from the calendar's own VTIMEZONE block (see ical.cpp).
// Historic DST changes are not modelled: the current rule is applied to all
// years, which is fine for a schedule that only looks 2 days back.
#pragma once
#include <stdint.h>

namespace tz {

struct Transition {
  uint8_t month = 3;   // 1..12
  uint8_t week = 5;    // 1..4, 5 = last
  uint8_t wday = 0;    // 0 = Sunday
  int32_t secs = 7200; // local time of the switch (seconds, may be negative)
};

struct Rule {
  int32_t stdOff = 0;  // seconds EAST of UTC (CET = +3600)
  int32_t dstOff = 0;
  bool hasDst = false;
  Transition start;    // std -> dst, in local standard time
  Transition end;      // dst -> std, in local daylight time
};

bool parsePosix(const char* s, Rule& out);

int32_t utcOffsetAt(const Rule& r, int64_t utc);

inline int64_t utcToLocal(const Rule& r, int64_t utc) { return utc + utcOffsetAt(r, utc); }

// Wall clock -> UTC. Ambiguous times (autumn) resolve to standard time and
// non-existent times (spring gap) use the standard offset, which is what
// pytz's localize(is_dst=False) did in the original.
int64_t localToUtc(const Rule& r, int64_t localSecs);

// POSIX rule for a TZID, or nullptr. Case-insensitive; path prefixes such as
// "/freeassociation.sourceforge.net/Europe/Ljubljana" are stripped.
const char* builtinPosix(const char* tzid);

void setDeviceRule(const char* posix);
const Rule& deviceRule();

}  // namespace tz
