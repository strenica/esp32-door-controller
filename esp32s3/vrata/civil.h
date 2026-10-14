// Proleptic Gregorian calendar arithmetic (no libc, no global TZ state).
// Day numbers are days since 1970-01-01; "local seconds" are civil wall-clock
// seconds counted as if the zone were UTC. Pure C++ so it also builds on a PC.
#pragma once
#include <stdint.h>

namespace civil {

inline int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms".
inline int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yy + (m <= 2));
}

// 0 = Monday .. 6 = Sunday (RFC 5545 order MO..SU). 1970-01-01 was a Thursday.
inline int weekdayMon0(int64_t days) { return static_cast<int>(floorMod(days + 3, 7)); }
// 0 = Sunday .. 6 = Saturday (POSIX TZ "Mm.w.d" order).
inline int weekdaySun0(int64_t days) { return static_cast<int>(floorMod(days + 4, 7)); }

inline bool isLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline unsigned daysInMonth(int64_t y, unsigned m) {
  static const uint8_t dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && isLeap(y)) ? 29u : dm[m - 1];
}

struct DateTime {
  int y = 1970;
  uint8_t mo = 1, d = 1, h = 0, mi = 0, s = 0;
};

inline int64_t toSeconds(const DateTime& t) {
  return daysFromCivil(t.y, t.mo, t.d) * 86400 + t.h * 3600L + t.mi * 60L + t.s;
}

inline DateTime fromSeconds(int64_t secs) {
  DateTime t;
  const int64_t days = floorDiv(secs, 86400);
  int64_t rem = secs - days * 86400;
  unsigned m, d;
  civilFromDays(days, t.y, m, d);
  t.mo = static_cast<uint8_t>(m);
  t.d = static_cast<uint8_t>(d);
  t.h = static_cast<uint8_t>(rem / 3600);
  rem %= 3600;
  t.mi = static_cast<uint8_t>(rem / 60);
  t.s = static_cast<uint8_t>(rem % 60);
  return t;
}

}  // namespace civil
