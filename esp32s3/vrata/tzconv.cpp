#include "tzconv.h"

#include <ctype.h>
#include <string.h>

#include "civil.h"

namespace tz {

namespace {

const char* parseName(const char* p) {
  if (*p == '<') {  // quoted form, e.g. <+03>
    ++p;
    while (*p && *p != '>') ++p;
    return *p == '>' ? p + 1 : nullptr;
  }
  const char* s = p;
  while (isalpha(static_cast<unsigned char>(*p))) ++p;
  return (p - s) >= 3 ? p : nullptr;
}

const char* parseNumber(const char* p, int& out) {
  if (!isdigit(static_cast<unsigned char>(*p))) return nullptr;
  out = 0;
  while (isdigit(static_cast<unsigned char>(*p))) out = out * 10 + (*p++ - '0');
  return p;
}

// [+|-]hh[:mm[:ss]]
const char* parseHms(const char* p, int32_t& out) {
  int sign = 1;
  if (*p == '+') {
    ++p;
  } else if (*p == '-') {
    sign = -1;
    ++p;
  }
  int h = 0, m = 0, s = 0;
  p = parseNumber(p, h);
  if (!p) return nullptr;
  if (*p == ':') {
    p = parseNumber(p + 1, m);
    if (!p) return nullptr;
    if (*p == ':') {
      p = parseNumber(p + 1, s);
      if (!p) return nullptr;
    }
  }
  out = sign * (h * 3600 + m * 60 + s);
  return p;
}

// Mm.w.d[/time]  (only the M form is used by real-world zones)
const char* parseTransition(const char* p, Transition& t) {
  if (*p != 'M') return nullptr;
  int m, w, d;
  p = parseNumber(p + 1, m);
  if (!p || *p != '.') return nullptr;
  p = parseNumber(p + 1, w);
  if (!p || *p != '.') return nullptr;
  p = parseNumber(p + 1, d);
  if (!p || m < 1 || m > 12 || w < 1 || w > 5 || d > 6) return nullptr;
  t.month = static_cast<uint8_t>(m);
  t.week = static_cast<uint8_t>(w);
  t.wday = static_cast<uint8_t>(d);
  t.secs = 7200;
  if (*p == '/') {
    int32_t v;
    p = parseHms(p + 1, v);
    if (!p) return nullptr;
    t.secs = v;
  }
  return p;
}

// Local wall-clock seconds at which the transition happens in `year`.
int64_t transitionLocal(const Transition& t, int year) {
  const int64_t first = civil::daysFromCivil(year, t.month, 1);
  int day = 1 + (t.wday - civil::weekdaySun0(first) + 7) % 7;
  day += (t.week - 1) * 7;
  const int dim = static_cast<int>(civil::daysInMonth(year, t.month));
  while (day > dim) day -= 7;
  return (first + day - 1) * 86400 + t.secs;
}

struct Entry {
  const char* tzid;
  const char* posix;
};

const char kCET[] = "CET-1CEST,M3.5.0,M10.5.0/3";
const char kEET[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
const char kWET[] = "WET0WEST,M3.5.0/1,M10.5.0";
const char kUK[] = "GMT0BST,M3.5.0/1,M10.5.0";
const char kUTC[] = "UTC0";

const Entry kZones[] = {
    // Central European Time
    {"Europe/Ljubljana", kCET}, {"Europe/Belgrade", kCET}, {"Europe/Zagreb", kCET},
    {"Europe/Sarajevo", kCET}, {"Europe/Skopje", kCET}, {"Europe/Podgorica", kCET},
    {"Europe/Vienna", kCET}, {"Europe/Berlin", kCET}, {"Europe/Busingen", kCET},
    {"Europe/Paris", kCET}, {"Europe/Rome", kCET}, {"Europe/Madrid", kCET},
    {"Europe/Amsterdam", kCET}, {"Europe/Brussels", kCET}, {"Europe/Luxembourg", kCET},
    {"Europe/Budapest", kCET}, {"Europe/Prague", kCET}, {"Europe/Bratislava", kCET},
    {"Europe/Warsaw", kCET}, {"Europe/Stockholm", kCET}, {"Europe/Oslo", kCET},
    {"Europe/Copenhagen", kCET}, {"Europe/Zurich", kCET}, {"Europe/Vaduz", kCET},
    {"Europe/Monaco", kCET}, {"Europe/Malta", kCET}, {"Europe/Andorra", kCET},
    {"Europe/Tirane", kCET}, {"Europe/San_Marino", kCET}, {"Europe/Vatican", kCET},
    {"Europe/Gibraltar", kCET}, {"Africa/Ceuta", kCET}, {"Arctic/Longyearbyen", kCET},
    {"CET", kCET}, {"MET", kCET},
    // Microsoft names (ical.py MICROSOFT_TZ_MAP + common Outlook exports)
    {"Central European Standard Time", kCET}, {"Central Europe Standard Time", kCET},
    {"W. Europe Standard Time", kCET}, {"Romance Standard Time", kCET},
    {"Greenwich Standard Time", kUK},  // ical.py mapped this to Europe/London
    {"GMT Standard Time", kUK},
    {"E. Europe Standard Time", kEET}, {"FLE Standard Time", kEET},
    {"GTB Standard Time", kEET},
    // Western / UK / Eastern Europe
    {"Europe/London", kUK}, {"Europe/Jersey", kUK}, {"Europe/Guernsey", kUK},
    {"Europe/Isle_of_Man", kUK}, {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", kWET}, {"Atlantic/Canary", kWET}, {"Atlantic/Madeira", kWET},
    {"Atlantic/Faroe", kWET}, {"WET", kWET},
    {"Europe/Athens", kEET}, {"Europe/Helsinki", kEET}, {"Europe/Kiev", kEET},
    {"Europe/Kyiv", kEET}, {"Europe/Riga", kEET}, {"Europe/Tallinn", kEET},
    {"Europe/Vilnius", kEET}, {"Europe/Sofia", kEET}, {"Europe/Bucharest", kEET},
    {"Europe/Mariehamn", kEET}, {"Asia/Nicosia", kEET}, {"Europe/Nicosia", kEET},
    {"EET", kEET},
    {"Europe/Chisinau", "EET-2EEST,M3.5.0,M10.5.0/3"},
    {"Europe/Kaliningrad", "EET-2"},
    {"Europe/Istanbul", "<+03>-3"}, {"Europe/Minsk", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},
    // UTC
    {"UTC", kUTC}, {"Etc/UTC", kUTC}, {"GMT", kUTC}, {"Etc/GMT", kUTC},
    {"Z", kUTC}, {"Coordinated Universal Time", kUTC},
    // A few others that show up in shared calendars
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Asia/Tokyo", "JST-9"}, {"Asia/Shanghai", "CST-8"}, {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dubai", "<+04>-4"}, {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};

bool equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

const char* lookup(const char* tzid) {
  for (const Entry& e : kZones) {
    if (equalsIgnoreCase(e.tzid, tzid)) return e.posix;
  }
  return nullptr;
}

Rule g_device;
bool g_deviceSet = false;

}  // namespace

bool parsePosix(const char* s, Rule& r) {
  r = Rule();
  if (!s) return false;
  const char* p = parseName(s);
  if (!p) return false;
  int32_t off;
  p = parseHms(p, off);
  if (!p) return false;
  r.stdOff = -off;  // POSIX offsets are WEST-positive
  if (*p == '\0') return true;
  p = parseName(p);
  if (!p) return false;
  r.hasDst = true;
  r.dstOff = r.stdOff + 3600;
  if (*p && *p != ',') {
    p = parseHms(p, off);
    if (!p) return false;
    r.dstOff = -off;
  }
  if (*p == '\0') {  // no explicit rule: assume the EU rule
    r.start = Transition{3, 5, 0, 7200};
    r.end = Transition{10, 5, 0, 10800};
    return true;
  }
  if (*p != ',') return false;
  p = parseTransition(p + 1, r.start);
  if (!p || *p != ',') return false;
  p = parseTransition(p + 1, r.end);
  return p && *p == '\0';
}

int32_t utcOffsetAt(const Rule& r, int64_t utc) {
  if (!r.hasDst) return r.stdOff;
  int y;
  unsigned m, d;
  civil::civilFromDays(civil::floorDiv(utc + r.stdOff, 86400), y, m, d);
  const int64_t startUtc = transitionLocal(r.start, y) - r.stdOff;
  const int64_t endUtc = transitionLocal(r.end, y) - r.dstOff;
  bool dst;
  if (startUtc < endUtc) {
    dst = utc >= startUtc && utc < endUtc;  // northern hemisphere
  } else {
    dst = !(utc >= endUtc && utc < startUtc);  // southern hemisphere
  }
  return dst ? r.dstOff : r.stdOff;
}

int64_t localToUtc(const Rule& r, int64_t local) {
  const int64_t asStd = local - r.stdOff;
  if (!r.hasDst || utcOffsetAt(r, asStd) == r.stdOff) return asStd;
  const int64_t asDst = local - r.dstOff;
  if (utcOffsetAt(r, asDst) == r.dstOff) return asDst;
  return asStd;  // non-existent wall time (spring gap)
}

const char* builtinPosix(const char* tzid) {
  if (!tzid || !*tzid) return nullptr;
  const char* hit = lookup(tzid);
  if (hit) return hit;
  // Strip leading path segments one at a time:
  // "/mozilla.org/20050126_1/Europe/Ljubljana" -> ... -> "Europe/Ljubljana"
  for (const char* p = strchr(tzid, '/'); p; p = strchr(p + 1, '/')) {
    if (p[1] == '\0') break;
    hit = lookup(p + 1);
    if (hit) return hit;
  }
  return nullptr;
}

void setDeviceRule(const char* posix) {
  if (!parsePosix(posix, g_device)) parsePosix(kCET, g_device);
  g_deviceSet = true;
}

const Rule& deviceRule() {
  if (!g_deviceSet) setDeviceRule(kCET);
  return g_device;
}

}  // namespace tz
