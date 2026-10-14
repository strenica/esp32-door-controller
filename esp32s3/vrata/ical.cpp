#include "ical.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>

#include "civil.h"

namespace ical {

uint32_t fnv1a(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

namespace {

const size_t kMaxLine = 8192;           // longer logical lines are truncated
const size_t kMaxListValues = 4000;     // EXDATE/RDATE values kept per event
const int64_t kMaxIterations = 200000;  // recurrence periods per event
const int64_t kOverrideMargin = 30L * 86400L;
const int kMaxWarnings = 20;
const uint8_t kFlagOverride = 1;

// ------------------------------------------------------------ helpers -----

bool equalsCI(const std::string& a, const char* b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (toupper(static_cast<unsigned char>(a[i])) != toupper(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool parseDigits(const char* p, int n, int& out) {
  out = 0;
  for (int i = 0; i < n; ++i) {
    if (!isdigit(static_cast<unsigned char>(p[i]))) return false;
    out = out * 10 + (p[i] - '0');
  }
  return true;
}

std::string unescapeText(const std::string& v) {
  std::string o;
  o.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    char c = v[i];
    if (c == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      o.push_back((n == 'n' || n == 'N') ? ' ' : n);
    } else {
      o.push_back(c);
    }
  }
  return o;
}

void copySummary(char* dst, size_t cap, const std::string& src) {
  size_t n = src.size();
  if (n >= cap) {
    n = cap - 1;
    // do not cut a UTF-8 sequence in half
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

// ------------------------------------------------------- line reading -----

class LineReader {
 public:
  explicit LineReader(ByteSource& s) : src_(s) {}

  // Next logical line with folding (CRLF + space/tab) undone.
  bool next(std::string& logical) {
    if (!hasPending_) {
      if (!readPhysical(pending_)) return false;
      hasPending_ = true;
    }
    for (;;) {
      if (!readPhysical(phys_)) {
        logical.swap(pending_);
        hasPending_ = false;
        return true;
      }
      if (!phys_.empty() && (phys_[0] == ' ' || phys_[0] == '\t')) {
        if (pending_.size() < kMaxLine) pending_.append(phys_, 1, std::string::npos);
        continue;
      }
      logical.swap(pending_);
      pending_.swap(phys_);
      return true;
    }
  }

 private:
  int getc() {
    if (pos_ >= len_) {
      if (eof_) return -1;
      int n = src_.read(buf_, sizeof(buf_));
      if (n <= 0) {
        eof_ = true;
        return -1;
      }
      len_ = static_cast<size_t>(n);
      pos_ = 0;
    }
    return buf_[pos_++];
  }

  bool readPhysical(std::string& line) {
    line.clear();
    int c = getc();
    if (c < 0) return false;
    while (c >= 0 && c != '\n') {
      if (line.size() < kMaxLine) line.push_back(static_cast<char>(c));
      c = getc();
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  }

  ByteSource& src_;
  uint8_t buf_[512];
  size_t len_ = 0;
  size_t pos_ = 0;
  bool eof_ = false;
  std::string pending_;
  std::string phys_;
  bool hasPending_ = false;
};

// Upper-cased property name ("DTSTART" from "DTSTART;TZID=...:...").
bool lineName(const std::string& l, std::string& name) {
  size_t i = 0;
  while (i < l.size() && l[i] != ';' && l[i] != ':') ++i;
  if (i == 0 || i >= l.size()) return false;
  name.assign(l, 0, i);
  for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return true;
}

struct Line {
  std::string params;  // text between name and ':' (quotes respected)
  std::string value;
};

bool splitLine(const std::string& l, Line& out) {
  size_t i = 0;
  const size_t n = l.size();
  while (i < n && l[i] != ';' && l[i] != ':') ++i;
  if (i >= n) return false;
  out.params.clear();
  if (l[i] == ';') {
    const size_t ps = i + 1;
    bool quoted = false;
    for (i = ps; i < n; ++i) {
      if (l[i] == '"') {
        quoted = !quoted;
      } else if (l[i] == ':' && !quoted) {
        break;
      }
    }
    if (i >= n) return false;
    out.params.assign(l, ps, i - ps);
  }
  out.value.assign(l, i + 1, std::string::npos);
  return true;
}

std::string getParam(const std::string& params, const char* key) {
  const size_t klen = strlen(key);
  size_t i = 0;
  const size_t n = params.size();
  while (i < n) {
    const size_t eq = params.find('=', i);
    if (eq == std::string::npos) break;
    bool match = (eq - i == klen);
    for (size_t k = 0; match && k < klen; ++k) {
      if (toupper(static_cast<unsigned char>(params[i + k])) != key[k]) match = false;
    }
    std::string val;
    size_t j = eq + 1;
    bool quoted = false;
    while (j < n) {
      const char c = params[j];
      if (c == '"') {
        quoted = !quoted;
      } else if (c == ';' && !quoted) {
        break;
      } else {
        val.push_back(c);
      }
      ++j;
    }
    if (match) return val;
    i = j + 1;
  }
  return std::string();
}

// ------------------------------------------------------------- values -----

struct DateVal {
  bool valid = false;
  bool isDate = false;  // VALUE=DATE (all-day)
  bool isUtc = false;   // trailing 'Z'
  std::string tzid;     // empty = UTC or floating
  civil::DateTime dt;
};

bool parseDateTime(const std::string& raw, DateVal& d) {
  const std::string s = trim(raw);
  d.valid = d.isDate = d.isUtc = false;
  int y, mo, dd, h = 0, mi = 0, se = 0;
  if (s.size() < 8) return false;
  const char* p = s.c_str();
  if (!parseDigits(p, 4, y) || !parseDigits(p + 4, 2, mo) || !parseDigits(p + 6, 2, dd)) return false;
  if (s.size() == 8) {
    d.isDate = true;
  } else {
    if (s.size() < 15 || (s[8] != 'T' && s[8] != 't')) return false;
    if (!parseDigits(p + 9, 2, h) || !parseDigits(p + 11, 2, mi) || !parseDigits(p + 13, 2, se)) return false;
    if (s.size() >= 16) {
      if (s[15] != 'Z' && s[15] != 'z') return false;
      d.isUtc = true;
    }
  }
  if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || h > 23 || mi > 59 || se > 60) return false;
  d.dt.y = y;
  d.dt.mo = static_cast<uint8_t>(mo);
  d.dt.d = static_cast<uint8_t>(dd);
  d.dt.h = static_cast<uint8_t>(h);
  d.dt.mi = static_cast<uint8_t>(mi);
  d.dt.s = static_cast<uint8_t>(se > 59 ? 59 : se);
  d.valid = true;
  return true;
}

bool parseDateProp(const Line& ln, DateVal& d) {
  std::string v = ln.value;
  const size_t comma = v.find(',');
  if (comma != std::string::npos) v.resize(comma);
  if (!parseDateTime(v, d)) return false;
  d.tzid = getParam(ln.params, "TZID");
  if (d.isUtc) d.tzid.clear();
  return true;
}

void parseDateList(const Line& ln, std::vector<DateVal>& out) {
  const std::string tzid = getParam(ln.params, "TZID");
  size_t start = 0;
  while (start <= ln.value.size() && out.size() < kMaxListValues) {
    size_t comma = ln.value.find(',', start);
    if (comma == std::string::npos) comma = ln.value.size();
    DateVal d;
    if (parseDateTime(ln.value.substr(start, comma - start), d)) {
      if (!d.isUtc) d.tzid = tzid;
      out.push_back(d);
    }
    start = comma + 1;
  }
}

// [+-]P[nW][nD][T[nH][nM][nS]]
bool parseDuration(const std::string& raw, int64_t& out) {
  const std::string s = trim(raw);
  size_t i = 0;
  int sign = 1;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    if (s[i] == '-') sign = -1;
    ++i;
  }
  if (i >= s.size() || toupper(static_cast<unsigned char>(s[i])) != 'P') return false;
  ++i;
  bool inTime = false, any = false;
  int64_t total = 0;
  while (i < s.size()) {
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
    if (c == 'T') {
      inTime = true;
      ++i;
      continue;
    }
    if (!isdigit(static_cast<unsigned char>(c))) return false;
    int64_t v = 0;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
    if (i >= s.size()) return false;
    const char u = static_cast<char>(toupper(static_cast<unsigned char>(s[i++])));
    switch (u) {
      case 'W': total += v * 604800; break;
      case 'D': total += v * 86400; break;
      case 'H': if (!inTime) return false; total += v * 3600; break;
      case 'M': if (!inTime) return false; total += v * 60; break;
      case 'S': if (!inTime) return false; total += v; break;
      default: return false;
    }
    any = true;
  }
  if (!any) return false;
  out = sign * total;
  return true;
}

// ±HHMM[SS]
bool parseUtcOffset(const std::string& raw, int32_t& out) {
  const std::string s = trim(raw);
  if (s.size() < 5 || (s[0] != '+' && s[0] != '-')) return false;
  int h, m, sec = 0;
  if (!parseDigits(s.c_str() + 1, 2, h) || !parseDigits(s.c_str() + 3, 2, m)) return false;
  if (s.size() >= 7 && !parseDigits(s.c_str() + 5, 2, sec)) return false;
  out = (s[0] == '-' ? -1 : 1) * (h * 3600 + m * 60 + sec);
  return true;
}

// -------------------------------------------------------------- RRULE -----

struct RRule {
  enum Freq { NONE, DAILY, WEEKLY, MONTHLY, YEARLY };
  Freq freq = NONE;
  int interval = 1;
  long count = -1;
  bool hasUntil = false;
  DateVal until;
  std::vector<std::pair<int, int>> byDay;  // (ordinal or 0, weekday 0=MO)
  std::vector<int> byMonthDay, byMonth, bySetPos;
  int wkst = 0;
  bool unsupported = false;
  std::string why;
};

int weekdayCode(const std::string& s) {
  static const char* kNames[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  if (s.size() != 2) return -1;
  for (int i = 0; i < 7; ++i) {
    if (toupper(static_cast<unsigned char>(s[0])) == kNames[i][0] &&
        toupper(static_cast<unsigned char>(s[1])) == kNames[i][1]) {
      return i;
    }
  }
  return -1;
}

void splitComma(const std::string& v, std::vector<std::string>& out) {
  out.clear();
  size_t start = 0;
  while (start <= v.size()) {
    size_t c = v.find(',', start);
    if (c == std::string::npos) c = v.size();
    std::string item = trim(v.substr(start, c - start));
    if (!item.empty()) out.push_back(item);
    start = c + 1;
  }
}

bool parseIntList(const std::string& v, std::vector<int>& out, int lo, int hi) {
  std::vector<std::string> items;
  splitComma(v, items);
  for (const std::string& it : items) {
    char* end = nullptr;
    const long x = strtol(it.c_str(), &end, 10);
    if (!end || *end != '\0' || x == 0 || x < lo || x > hi) return false;
    out.push_back(static_cast<int>(x));
  }
  return !out.empty();
}

void parseRRule(const std::string& s, RRule& r) {
  r = RRule();
  size_t start = 0;
  while (start < s.size()) {
    size_t semi = s.find(';', start);
    if (semi == std::string::npos) semi = s.size();
    const std::string part = s.substr(start, semi - start);
    start = semi + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(part.substr(0, eq));
    for (char& c : key) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const std::string val = trim(part.substr(eq + 1));
    bool bad = false;
    if (key == "FREQ") {
      if (equalsCI(val, "DAILY")) r.freq = RRule::DAILY;
      else if (equalsCI(val, "WEEKLY")) r.freq = RRule::WEEKLY;
      else if (equalsCI(val, "MONTHLY")) r.freq = RRule::MONTHLY;
      else if (equalsCI(val, "YEARLY")) r.freq = RRule::YEARLY;
      else bad = true;
    } else if (key == "INTERVAL") {
      r.interval = atoi(val.c_str());
      if (r.interval < 1) r.interval = 1;
    } else if (key == "COUNT") {
      r.count = atol(val.c_str());
      if (r.count < 0) r.count = 0;
    } else if (key == "UNTIL") {
      r.hasUntil = parseDateTime(val, r.until);
      bad = !r.hasUntil;
    } else if (key == "BYDAY") {
      std::vector<std::string> items;
      splitComma(val, items);
      for (const std::string& it : items) {
        if (it.size() < 2) {
          bad = true;
          break;
        }
        const int wd = weekdayCode(it.substr(it.size() - 2));
        const std::string ordStr = it.substr(0, it.size() - 2);
        int ord = 0;
        if (!ordStr.empty()) {
          char* end = nullptr;
          ord = static_cast<int>(strtol(ordStr.c_str(), &end, 10));
          if (!end || *end != '\0' || ord == 0 || ord < -53 || ord > 53) bad = true;
        }
        if (wd < 0) bad = true;
        if (bad) break;
        r.byDay.push_back(std::make_pair(ord, wd));
      }
    } else if (key == "BYMONTHDAY") {
      bad = !parseIntList(val, r.byMonthDay, -31, 31);
    } else if (key == "BYMONTH") {
      bad = !parseIntList(val, r.byMonth, 1, 12);
    } else if (key == "BYSETPOS") {
      bad = !parseIntList(val, r.bySetPos, -366, 366);
    } else if (key == "WKST") {
      const int wd = weekdayCode(val);
      if (wd >= 0) r.wkst = wd;
    } else if (key == "BYHOUR" || key == "BYMINUTE" || key == "BYSECOND" || key == "BYWEEKNO" ||
               key == "BYYEARDAY") {
      bad = true;
    }
    if (bad && !r.unsupported) {
      r.unsupported = true;
      r.why = part;
    }
  }
  if (r.freq == RRule::NONE && !r.unsupported) {
    r.unsupported = true;
    r.why = "missing/unsupported FREQ";
  }
  // Ordinals only make sense for MONTHLY/YEARLY; RFC 5545 forbids them otherwise.
  if (r.freq == RRule::DAILY || r.freq == RRule::WEEKLY) {
    for (auto& p : r.byDay) p.first = 0;
  }
}

// ---------------------------------------------------- expansion helpers ---

struct Zone {
  const tz::Rule* rule = nullptr;  // nullptr = UTC
  int64_t toUtc(int64_t local) const { return rule ? tz::localToUtc(*rule, local) : local; }
  int64_t toLocal(int64_t utc) const { return rule ? tz::utcToLocal(*rule, utc) : utc; }
};

struct Until {
  int kind = 0;  // 0 none, 1 UTC instant, 2 local seconds, 3 local day number
  int64_t v = 0;
  bool beyond(int64_t local, const Zone& z) const {
    switch (kind) {
      case 1: return z.toUtc(local) > v;
      case 2: return local > v;
      case 3: return civil::floorDiv(local, 86400) > v;
      default: return false;
    }
  }
};

bool containsInt(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

bool monthDayMatches(const std::vector<int>& mds, int dim, int d) {
  for (int md : mds) {
    if ((md > 0 ? md : dim + md + 1) == d) return true;
  }
  return false;
}

// Weekday `wd` inside [first, last]: every one (ord == 0), the ord-th (ord > 0)
// or the ord-th from the end (ord < 0).
void addNthWeekdays(int64_t first, int64_t last, int ord, int wd, std::vector<int64_t>& out) {
  if (ord == 0) {
    for (int64_t d = first + (wd - civil::weekdayMon0(first) + 7) % 7; d <= last; d += 7) out.push_back(d);
  } else if (ord > 0) {
    const int64_t d = first + (wd - civil::weekdayMon0(first) + 7) % 7 + 7L * (ord - 1);
    if (d <= last) out.push_back(d);
  } else {
    const int64_t d = last - (civil::weekdayMon0(last) - wd + 7) % 7 - 7L * (-ord - 1);
    if (d >= first) out.push_back(d);
  }
}

void dayCandidates(const RRule& r, int64_t day, std::vector<int64_t>& out) {
  int y;
  unsigned m, d;
  civil::civilFromDays(day, y, m, d);
  if (!r.byMonth.empty() && !containsInt(r.byMonth, static_cast<int>(m))) return;
  if (!r.byMonthDay.empty() &&
      !monthDayMatches(r.byMonthDay, static_cast<int>(civil::daysInMonth(y, m)), static_cast<int>(d))) {
    return;
  }
  if (!r.byDay.empty()) {
    const int wd = civil::weekdayMon0(day);
    bool ok = false;
    for (const auto& p : r.byDay) ok = ok || p.second == wd;
    if (!ok) return;
  }
  out.push_back(day);
}

void weekCandidates(const RRule& r, int64_t weekStart, int wd0, std::vector<int64_t>& out) {
  if (r.byDay.empty()) {
    out.push_back(weekStart + (wd0 - r.wkst + 7) % 7);
  } else {
    for (const auto& p : r.byDay) out.push_back(weekStart + (p.second - r.wkst + 7) % 7);
  }
  if (!r.byMonth.empty()) {
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](int64_t day) {
                               int y;
                               unsigned m, d;
                               civil::civilFromDays(day, y, m, d);
                               return !containsInt(r.byMonth, static_cast<int>(m));
                             }),
              out.end());
  }
}

void monthCandidates(const RRule& r, int y, unsigned m, unsigned defaultDay, std::vector<int64_t>& out) {
  if (!r.byMonth.empty() && !containsInt(r.byMonth, static_cast<int>(m))) return;
  const int dim = static_cast<int>(civil::daysInMonth(y, m));
  const int64_t first = civil::daysFromCivil(y, m, 1);
  const int64_t last = first + dim - 1;
  const bool md = !r.byMonthDay.empty();
  const bool bd = !r.byDay.empty();
  if (!md && !bd) {
    if (static_cast<int>(defaultDay) <= dim) out.push_back(first + defaultDay - 1);
    return;
  }
  if (bd) {
    const size_t mark = out.size();
    for (const auto& p : r.byDay) addNthWeekdays(first, last, p.first, p.second, out);
    if (md) {  // BYDAY + BYMONTHDAY: both must match (e.g. "Friday the 13th")
      out.erase(std::remove_if(out.begin() + mark, out.end(),
                               [&](int64_t day) {
                                 return !monthDayMatches(r.byMonthDay, dim, static_cast<int>(day - first + 1));
                               }),
                out.end());
    }
    return;
  }
  for (int v : r.byMonthDay) {
    const int d = v > 0 ? v : dim + v + 1;
    if (d >= 1 && d <= dim) out.push_back(first + d - 1);
  }
}

void yearCandidates(const RRule& r, int y, unsigned m0, unsigned d0, std::vector<int64_t>& out) {
  const bool md = !r.byMonthDay.empty();
  const bool bd = !r.byDay.empty();
  if (!r.byMonth.empty()) {
    for (int m : r.byMonth) monthCandidates(r, y, static_cast<unsigned>(m), d0, out);
  } else if (bd && !md) {  // e.g. BYDAY=20MO: 20th Monday of the year
    const int64_t first = civil::daysFromCivil(y, 1, 1);
    const int64_t last = civil::daysFromCivil(y, 12, 31);
    for (const auto& p : r.byDay) addNthWeekdays(first, last, p.first, p.second, out);
  } else if (md) {
    for (unsigned m = 1; m <= 12; ++m) monthCandidates(r, y, m, d0, out);
  } else if (d0 <= civil::daysInMonth(y, m0)) {  // Feb 29 only in leap years
    out.push_back(civil::daysFromCivil(y, m0, d0));
  }
}

void applySetPos(const RRule& r, std::vector<int64_t>& days) {
  if (r.bySetPos.empty() || days.empty()) return;
  std::vector<int64_t> sel;
  const int n = static_cast<int>(days.size());
  for (int p : r.bySetPos) {
    const int idx = p > 0 ? p - 1 : n + p;
    if (idx >= 0 && idx < n) sel.push_back(days[idx]);
  }
  std::sort(sel.begin(), sel.end());
  sel.erase(std::unique(sel.begin(), sel.end()), sel.end());
  days.swap(sel);
}

// Calls emit(localSeconds) for DTSTART and every later instance, in order,
// until the rule ends, emit() returns false or the window is passed.
// ffTarget: without COUNT, periods ending well before this are skipped.
template <typename Emit>
bool expandRule(const RRule& r, int64_t dtLocal, const Zone& z, const Until& until, int64_t ffTarget,
                int64_t stopLocal, Emit& emit, void (*yieldFn)()) {
  const int64_t startDay = civil::floorDiv(dtLocal, 86400);
  const int64_t tod = dtLocal - startDay * 86400;
  int y0;
  unsigned m0, d0;
  civil::civilFromDays(startDay, y0, m0, d0);
  const int wd0 = civil::weekdayMon0(startDay);
  const int64_t weekStart0 = startDay - (wd0 - r.wkst + 7) % 7;
  const int64_t iv = r.interval;

  long emitted = 1;  // DTSTART always counts as the first instance
  if (!emit(dtLocal)) return true;
  if (r.count >= 0 && emitted >= r.count) return true;

  int64_t k = 0;
  if (r.count < 0 && ffTarget > dtLocal) {
    const int64_t tDay = civil::floorDiv(ffTarget, 86400);
    int ty;
    unsigned tm, td;
    civil::civilFromDays(tDay, ty, tm, td);
    switch (r.freq) {
      case RRule::DAILY: k = (tDay - startDay) / iv; break;
      case RRule::WEEKLY: k = ((tDay - weekStart0) / 7) / iv; break;
      case RRule::MONTHLY: k = ((static_cast<int64_t>(ty) - y0) * 12 + (static_cast<int>(tm) - static_cast<int>(m0))) / iv; break;
      case RRule::YEARLY: k = (static_cast<int64_t>(ty) - y0) / iv; break;
      default: break;
    }
    k = k > 1 ? k - 1 : 0;
  }

  std::vector<int64_t> days;
  days.reserve(16);
  for (int64_t iter = 0; iter < kMaxIterations; ++iter, ++k) {
    if (yieldFn && (iter & 1023) == 1023) yieldFn();
    days.clear();
    int64_t periodFirst = 0;
    switch (r.freq) {
      case RRule::DAILY:
        periodFirst = startDay + k * iv;
        dayCandidates(r, periodFirst, days);
        break;
      case RRule::WEEKLY:
        periodFirst = weekStart0 + 7 * k * iv;
        weekCandidates(r, periodFirst, wd0, days);
        break;
      case RRule::MONTHLY: {
        const int64_t mi = static_cast<int64_t>(y0) * 12 + (m0 - 1) + k * iv;
        const int y = static_cast<int>(civil::floorDiv(mi, 12));
        const unsigned m = static_cast<unsigned>(mi - static_cast<int64_t>(y) * 12) + 1;
        periodFirst = civil::daysFromCivil(y, m, 1);
        monthCandidates(r, y, m, d0, days);
        break;
      }
      case RRule::YEARLY: {
        const int y = y0 + static_cast<int>(k * iv);
        periodFirst = civil::daysFromCivil(y, 1, 1);
        yearCandidates(r, y, m0, d0, days);
        break;
      }
      default:
        return true;
    }
    if (periodFirst * 86400 > stopLocal) return true;
    std::sort(days.begin(), days.end());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    applySetPos(r, days);
    for (int64_t day : days) {
      const int64_t local = day * 86400 + tod;
      if (local <= dtLocal) continue;
      if (until.beyond(local, z)) return true;
      if (r.count >= 0 && emitted >= r.count) return true;
      ++emitted;
      if (!emit(local)) return true;
    }
  }
  return false;  // iteration limit hit
}

// ------------------------------------------------------------- parser -----

enum Comp : uint8_t { C_OTHER, C_VCALENDAR, C_VEVENT, C_VTIMEZONE, C_STANDARD, C_DAYLIGHT };

Comp compOf(const std::string& v) {
  const std::string t = trim(v);
  if (equalsCI(t, "VEVENT")) return C_VEVENT;
  if (equalsCI(t, "VCALENDAR")) return C_VCALENDAR;
  if (equalsCI(t, "VTIMEZONE")) return C_VTIMEZONE;
  if (equalsCI(t, "STANDARD")) return C_STANDARD;
  if (equalsCI(t, "DAYLIGHT")) return C_DAYLIGHT;
  return C_OTHER;  // VALARM, VTODO, ... (their properties are ignored)
}

struct RawEvent {
  std::string uid, summary, status, rrule, duration;
  DateVal dtstart, dtend, recurrenceId;
  bool hasDtend = false, hasRecId = false, hasDuration = false;
  std::vector<DateVal> exdates, rdates;

  void clear() {
    uid.clear();
    summary.clear();
    status.clear();
    rrule.clear();
    duration.clear();
    dtstart = DateVal();
    dtend = DateVal();
    recurrenceId = DateVal();
    hasDtend = hasRecId = hasDuration = false;
    exdates.clear();
    rdates.clear();
  }
};

struct VtzPart {
  bool present = false;
  int64_t since = INT64_MIN;
  int32_t offsetTo = 0;
  civil::DateTime dt;
  std::string rrule;
};

struct VtzBuild {
  std::string tzid;
  VtzPart std, dst;
};

struct OverrideKey {
  uint32_t uid;
  int64_t recid;
  bool operator<(const OverrideKey& o) const { return uid != o.uid ? uid < o.uid : recid < o.recid; }
};

// VTIMEZONE STANDARD/DAYLIGHT RRULE -> POSIX-style transition.
bool transitionFrom(const std::string& rrule, const civil::DateTime& dt, tz::Transition& t) {
  if (rrule.empty()) return false;
  RRule rr;
  parseRRule(rrule, rr);
  if (rr.freq != RRule::YEARLY || rr.byMonth.size() != 1 || rr.byDay.size() != 1) return false;
  int ord = rr.byDay[0].first;
  if (ord == 0) {  // old Outlook style: BYDAY=SU;BYMONTHDAY=8,9,10,11,12,13,14
    if (rr.byMonthDay.empty()) return false;
    const int first = *std::min_element(rr.byMonthDay.begin(), rr.byMonthDay.end());
    if (first <= 0) return false;
    ord = first >= 25 ? 5 : (first - 1) / 7 + 1;
  } else if (ord < 0) {
    if (ord != -1) return false;
    ord = 5;
  } else if (ord > 5) {
    return false;
  }
  t.month = static_cast<uint8_t>(rr.byMonth[0]);
  t.week = static_cast<uint8_t>(ord);
  t.wday = static_cast<uint8_t>((rr.byDay[0].second + 1) % 7);  // MO=0 -> SU=0
  t.secs = dt.h * 3600 + dt.mi * 60 + dt.s;
  return true;
}

class Parser {
 public:
  Parser(const Options& o, std::vector<Occurrence>& out, Stats& st) : opt_(o), out_(out), st_(st) {}

  bool run(ByteSource& src) {
    LineReader rd(src);
    std::string l, name;
    Line ln;
    std::vector<Comp> stack;
    stack.reserve(8);
    bool first = true;
    while (rd.next(l)) {
      ++st_.lines;
      if (opt_.yield && (st_.lines & 255) == 0) opt_.yield();
      if (first) {
        first = false;
        if (l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF && static_cast<unsigned char>(l[1]) == 0xBB &&
            static_cast<unsigned char>(l[2]) == 0xBF) {
          l.erase(0, 3);  // UTF-8 BOM
        }
        l = trim(l);
        if (!equalsCI(l, "BEGIN:VCALENDAR")) return false;
        st_.validHeader = true;
      }
      if (l.empty() || !lineName(l, name)) continue;
      if (name == "BEGIN") {
        if (!splitLine(l, ln)) continue;
        const Comp c = compOf(ln.value);
        if (stack.size() < 16) stack.push_back(c);
        onBegin(c);
        continue;
      }
      if (name == "END") {
        if (stack.empty()) continue;
        const Comp c = stack.back();
        stack.pop_back();
        onEnd(c);
        continue;
      }
      if (stack.empty()) continue;
      const Comp top = stack.back();
      if (top == C_VEVENT) {
        onEventProp(name, l, ln);
      } else if (top == C_VTIMEZONE) {
        if (name == "TZID" && splitLine(l, ln)) curTz_.tzid = trim(ln.value);
      } else if ((top == C_STANDARD || top == C_DAYLIGHT) && stack.size() >= 2 &&
                 stack[stack.size() - 2] == C_VTIMEZONE) {
        onTzPartProp(name, l, ln);
      }
    }
    return st_.validHeader;
  }

  // A VTIMEZONE defined after the events that use it -> parse once more.
  bool needsReparse() const {
    for (const std::string& id : unknown_) {
      if (vtz_.count(id)) return true;
    }
    return false;
  }

  void prepareReparse() {
    out_.clear();
    overrides_.clear();
    cache_.clear();
    unknown_.clear();
    ev_.clear();
    st_ = Stats();
    st_.reparsed = true;
  }

  void finish() {
    if (!overrides_.empty()) {
      std::sort(overrides_.begin(), overrides_.end());
      out_.erase(std::remove_if(out_.begin(), out_.end(),
                                [&](const Occurrence& o) {
                                  return !(o.flags & kFlagOverride) &&
                                         std::binary_search(overrides_.begin(), overrides_.end(),
                                                            OverrideKey{o.uid, o.start});
                                }),
                 out_.end());
    }
    compact();
    st_.occurrences = static_cast<uint32_t>(out_.size());
  }

 private:
  void warnf(const char* fmt, ...) {
    if (!opt_.warn || warnings_ >= kMaxWarnings) return;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    opt_.warn(buf);
    if (++warnings_ == kMaxWarnings) opt_.warn("further calendar warnings suppressed");
  }

  void onBegin(Comp c) {
    if (c == C_VEVENT) {
      ev_.clear();
    } else if (c == C_VTIMEZONE) {
      curTz_ = VtzBuild();
    } else if (c == C_STANDARD || c == C_DAYLIGHT) {
      curPart_ = VtzPart();
      curPart_.present = true;
    }
  }

  void onEnd(Comp c) {
    if (c == C_VEVENT) {
      processEvent();
    } else if (c == C_VTIMEZONE) {
      buildVtz();
    } else if (c == C_STANDARD || c == C_DAYLIGHT) {
      VtzPart& slot = (c == C_STANDARD) ? curTz_.std : curTz_.dst;
      if (!slot.present || curPart_.since >= slot.since) slot = curPart_;  // keep the newest rule
    }
  }

  void onEventProp(const std::string& name, const std::string& l, Line& ln) {
    if (name == "DTSTART") {
      if (splitLine(l, ln)) parseDateProp(ln, ev_.dtstart);
    } else if (name == "DTEND") {
      if (splitLine(l, ln)) ev_.hasDtend = parseDateProp(ln, ev_.dtend);
    } else if (name == "DURATION") {
      if (splitLine(l, ln)) {
        ev_.duration = ln.value;
        ev_.hasDuration = true;
      }
    } else if (name == "RRULE") {
      if (splitLine(l, ln) && ev_.rrule.empty()) ev_.rrule = ln.value;
    } else if (name == "EXDATE") {
      if (splitLine(l, ln)) parseDateList(ln, ev_.exdates);
    } else if (name == "RDATE") {
      if (splitLine(l, ln) && !equalsCI(getParam(ln.params, "VALUE"), "PERIOD")) parseDateList(ln, ev_.rdates);
    } else if (name == "RECURRENCE-ID") {
      if (splitLine(l, ln)) ev_.hasRecId = parseDateProp(ln, ev_.recurrenceId);
    } else if (name == "UID") {
      if (splitLine(l, ln)) ev_.uid = trim(ln.value);
    } else if (name == "SUMMARY") {
      if (splitLine(l, ln)) ev_.summary = unescapeText(ln.value);
    } else if (name == "STATUS") {
      if (splitLine(l, ln)) ev_.status = trim(ln.value);
    }
  }

  void onTzPartProp(const std::string& name, const std::string& l, Line& ln) {
    if (!splitLine(l, ln)) return;
    if (name == "DTSTART") {
      DateVal d;
      if (parseDateTime(ln.value, d)) {
        curPart_.dt = d.dt;
        curPart_.since = civil::toSeconds(d.dt);
      }
    } else if (name == "TZOFFSETTO") {
      parseUtcOffset(ln.value, curPart_.offsetTo);
    } else if (name == "RRULE") {
      curPart_.rrule = ln.value;
    }
  }

  void buildVtz() {
    if (curTz_.tzid.empty() || tz::builtinPosix(curTz_.tzid.c_str())) return;  // built-in table wins
    tz::Rule r;
    const VtzPart& s = curTz_.std;
    const VtzPart& d = curTz_.dst;
    if (s.present && d.present && transitionFrom(d.rrule, d.dt, r.start) && transitionFrom(s.rrule, s.dt, r.end)) {
      r.hasDst = true;
      r.stdOff = s.offsetTo;
      r.dstOff = d.offsetTo;
    } else if (s.present) {
      r.stdOff = s.offsetTo;
    } else if (d.present) {
      r.stdOff = d.offsetTo;
    } else {
      return;
    }
    vtz_[curTz_.tzid] = r;
  }

  const tz::Rule* resolveTz(const std::string& tzid) {
    auto it = cache_.find(tzid);
    if (it != cache_.end()) return &it->second;
    tz::Rule r;
    const char* posix = tz::builtinPosix(tzid.c_str());
    bool ok = posix && tz::parsePosix(posix, r);
    if (!ok) {
      auto v = vtz_.find(tzid);
      if (v != vtz_.end()) {
        r = v->second;
        ok = true;
      }
    }
    if (!ok) {
      if (unknown_.insert(tzid).second) warnf("unknown TZID '%s': its events are skipped", tzid.c_str());
      return nullptr;
    }
    return &(cache_[tzid] = r);
  }

  // DATE / DATE-TIME value -> UTC. Values without TZID and 'Z' use `z`.
  bool toUtc(const DateVal& d, const Zone& z, int64_t& out) {
    const int64_t local = civil::toSeconds(d.dt);
    if (d.isUtc) {
      out = local;
      return true;
    }
    if (!d.tzid.empty()) {
      const tz::Rule* r = resolveTz(d.tzid);
      if (!r) return false;
      out = tz::localToUtc(*r, local);
      return true;
    }
    out = z.toUtc(local);
    return true;
  }

  void add(int64_t s, int64_t e, uint32_t uid, const std::string& summary, uint8_t flags) {
    Occurrence o;
    o.start = s;
    o.end = e;
    o.uid = uid;
    o.flags = flags;
    std::string name, user, pin;
    if (parseBooking(summary, name, user, pin)) {
      copySummary(o.summary, sizeof(o.summary), name + " (PIN)");  // never show the PIN
      copySummary(o.user, sizeof(o.user), user);
      copySummary(o.pin, sizeof(o.pin), pin);
    } else {
      copySummary(o.summary, sizeof(o.summary), summary);
      o.user[0] = '\0';
      o.pin[0] = '\0';
    }
    out_.push_back(o);
    if (out_.size() >= opt_.maxOccurrences + 100) compact();  // bounded RAM while parsing
  }

  bool haveOccurrence(uint32_t uid, int64_t start) const {
    for (const Occurrence& o : out_) {
      if (o.uid == uid && o.start == start) return true;
    }
    return false;
  }

  // Sort by start and keep the earliest maxOccurrences.
  void compact() {
    std::sort(out_.begin(), out_.end(), [](const Occurrence& a, const Occurrence& b) {
      return a.start != b.start ? a.start < b.start : a.end < b.end;
    });
    if (out_.size() > opt_.maxOccurrences) {
      out_.resize(opt_.maxOccurrences);
      st_.truncated = true;
    }
  }

  void processEvent() {
    RawEvent& ev = ev_;
    ++st_.events;
    if (!ev.dtstart.valid) return;

    Zone z;  // zone in which DTSTART's wall-clock time is interpreted
    if (ev.dtstart.isUtc) {
      z.rule = nullptr;
    } else if (!ev.dtstart.tzid.empty()) {
      z.rule = resolveTz(ev.dtstart.tzid);
      if (!z.rule) {
        ++st_.skippedTz;  // same as ical.py: unrecognized timezone -> skip
        return;
      }
    } else {
      z.rule = opt_.deviceTz;  // floating time / all-day date: device local time
    }

    const bool allDay = ev.dtstart.isDate;
    const uint32_t uid = fnv1a(ev.uid);
    const bool cancelled = equalsCI(ev.status, "CANCELLED");
    const int64_t dtLocal = civil::toSeconds(ev.dtstart.dt);
    const int64_t startUtc = z.toUtc(dtLocal);

    int64_t dur = opt_.defaultDurationS;  // ical.py: no DTEND -> start + 1 hour
    int64_t allDayDays = 1;
    if (allDay) {
      if (ev.hasDtend && ev.dtend.valid) {
        const int64_t d = civil::floorDiv(civil::toSeconds(ev.dtend.dt), 86400) - civil::floorDiv(dtLocal, 86400);
        if (d > 0) allDayDays = d;
      } else if (ev.hasDuration) {
        int64_t s;
        if (parseDuration(ev.duration, s) && s >= 86400) allDayDays = s / 86400;
      }
    } else if (ev.hasDtend && ev.dtend.valid) {
      int64_t e;
      if (!toUtc(ev.dtend, z, e)) {
        ++st_.skippedTz;
        return;
      }
      dur = e - startUtc;
      if (dur < 0) dur = 0;
    } else if (ev.hasDuration) {
      int64_t s;
      if (parseDuration(ev.duration, s) && s >= 0) dur = s;
    }

    const bool isOverride = ev.hasRecId && ev.recurrenceId.valid;
    if (isOverride) {
      // A modified/cancelled single instance of a recurring event: remember
      // which master instance it replaces, then add it on its own.
      ++st_.overrides;
      int64_t rid;
      if (toUtc(ev.recurrenceId, z, rid) && rid >= opt_.windowStart - kOverrideMargin &&
          rid <= opt_.windowEnd + kOverrideMargin) {
        overrides_.push_back(OverrideKey{uid, rid});
      }
    }
    if (cancelled) {
      ++st_.cancelled;
      return;
    }
    if (allDay && !opt_.allDayActivates) {
      ++st_.skippedAllDay;
      return;
    }

    std::vector<int64_t> exUtc, exDays;
    for (const DateVal& x : ev.exdates) {
      if (!x.valid) continue;
      if (x.isDate) {
        exDays.push_back(civil::floorDiv(civil::toSeconds(x.dt), 86400));
      } else {
        int64_t u;
        if (toUtc(x, z, u)) exUtc.push_back(u);
      }
    }
    std::sort(exUtc.begin(), exUtc.end());
    std::sort(exDays.begin(), exDays.end());

    const uint8_t flags = isOverride ? kFlagOverride : 0;
    auto emit = [&](int64_t local) -> bool {
      const int64_t s = z.toUtc(local);
      if (s > opt_.windowEnd) return false;  // instances are ascending: stop
      const int64_t e = allDay ? z.toUtc(local + allDayDays * 86400) - 1 : s + dur;
      if (e < opt_.windowStart || e < opt_.dropEndedBefore) return true;
      if (!exUtc.empty() && std::binary_search(exUtc.begin(), exUtc.end(), s)) return true;
      if (!exDays.empty() && std::binary_search(exDays.begin(), exDays.end(), civil::floorDiv(local, 86400))) {
        return true;
      }
      add(s, e, uid, ev.summary, flags);
      return true;
    };

    bool recurring = false;
    RRule rr;
    if (!isOverride && !ev.rrule.empty()) {
      parseRRule(ev.rrule, rr);
      if (rr.unsupported) {
        ++st_.unsupportedRules;
        warnf("'%.40s': unsupported RRULE part '%.40s', only the first instance is used", ev.summary.c_str(),
              rr.why.c_str());
      } else {
        recurring = true;
      }
    }

    if (recurring) {
      ++st_.recurring;
      Until until;
      if (rr.hasUntil) {
        if (rr.until.isUtc) {
          until.kind = 1;
          until.v = civil::toSeconds(rr.until.dt);
        } else if (rr.until.isDate) {
          until.kind = 3;
          until.v = civil::floorDiv(civil::toSeconds(rr.until.dt), 86400);
        } else {
          until.kind = 2;
          until.v = civil::toSeconds(rr.until.dt);
        }
      }
      const int64_t span = allDay ? allDayDays * 86400 : dur;
      const int64_t ffTarget = opt_.windowStart - span - 2 * 86400;
      const int64_t stopLocal = opt_.windowEnd + 2 * 86400;
      if (!expandRule(rr, dtLocal, z, until, ffTarget, stopLocal, emit, opt_.yield)) {
        warnf("'%.40s': recurrence too long, expansion stopped", ev.summary.c_str());
      }
    } else {
      emit(dtLocal);
    }

    // RDATE: extra instances with the same length
    for (const DateVal& r : ev.rdates) {
      if (!r.valid) continue;
      int64_t local;
      if (r.isUtc || !r.tzid.empty()) {
        int64_t u;
        if (!toUtc(r, z, u)) continue;
        local = z.toLocal(u);
      } else {
        local = civil::toSeconds(r.dt);
        if (r.isDate && !allDay) local += dtLocal - civil::floorDiv(dtLocal, 86400) * 86400;
      }
      if (haveOccurrence(uid, z.toUtc(local))) continue;
      emit(local);
    }
  }

  const Options& opt_;
  std::vector<Occurrence>& out_;
  Stats& st_;
  RawEvent ev_;
  VtzBuild curTz_;
  VtzPart curPart_;
  std::map<std::string, tz::Rule> vtz_;    // rules from VTIMEZONE blocks
  std::map<std::string, tz::Rule> cache_;  // resolved TZIDs
  std::set<std::string> unknown_;
  std::vector<OverrideKey> overrides_;
  int warnings_ = 0;
};

// Same table as normalize_username() in server/oss/lib/doors.php.
char mapCodepoint(uint32_t cp) {
  if (cp < 0x80) return isalnum(static_cast<int>(cp)) ? static_cast<char>(tolower(static_cast<int>(cp))) : 0;
  if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5)) return 'a';
  if (cp == 0xC7 || cp == 0xE7 || cp == 0x10C || cp == 0x10D || cp == 0x106 || cp == 0x107) return 'c';
  if (cp == 0x110 || cp == 0x111) return 'd';
  if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) return 'e';
  if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) return 'i';
  if (cp == 0xD1 || cp == 0xF1) return 'n';
  if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8 || (cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) return 'o';
  if (cp == 0x160 || cp == 0x161 || cp == 0xDF) return 's';
  if ((cp >= 0xD9 && cp <= 0xDC) || (cp >= 0xF9 && cp <= 0xFC)) return 'u';
  if (cp == 0xDD || cp == 0xFD || cp == 0xFF) return 'y';
  if (cp == 0x17D || cp == 0x17E) return 'z';
  return 0;  // everything else (spaces, punctuation, other scripts) is dropped
}

}  // namespace

std::string normalizeUser(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size() && out.size() < 20) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;  // emoji etc.: dropped
    }
    i += len;
    const char m = cp ? mapCodepoint(cp) : 0;
    if (m) out.push_back(m);
  }
  return out;
}

bool parseBooking(const std::string& summary, std::string& name, std::string& user, std::string& pin) {
  const size_t dash = summary.rfind('-');
  if (dash == std::string::npos) return false;
  const std::string right = trim(summary.substr(dash + 1));
  if (right.size() < 4 || right.size() > 8) return false;
  for (char c : right) {
    if (!isdigit(static_cast<unsigned char>(c))) return false;
  }
  const std::string left = trim(summary.substr(0, dash));
  const std::string u = normalizeUser(left);
  if (left.empty() || u.empty()) return false;
  name = left;
  user = u;
  pin = right;
  return true;
}

bool safeEquals(const char* a, const char* b) {
  const size_t la = strlen(a), lb = strlen(b);
  const size_t n = la > lb ? la : lb;
  unsigned char d = la != lb;
  for (size_t i = 0; i < n; ++i) {
    d |= static_cast<unsigned char>((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
  }
  return d == 0;
}

const Occurrence* findBooking(const std::vector<Occurrence>& occ, const std::string& user, const std::string& pin,
                              int64_t now, int64_t beforeS, int64_t afterS) {
  const Occurrence* hit = nullptr;
  for (const Occurrence& o : occ) {
    if (!o.user[0] || now < o.start - beforeS || now > o.end + afterS) continue;
    const bool ok = safeEquals(o.pin, pin.c_str()) && user == o.user;
    if (ok && !hit) hit = &o;  // keep scanning: timing does not reveal which entry matched
  }
  return hit;
}

bool expand(ByteSource& src, const Options& opt, std::vector<Occurrence>& out, Stats& stats) {
  out.clear();
  stats = Stats();
  Parser p(opt, out, stats);
  if (!p.run(src)) return false;
  if (p.needsReparse() && src.rewind()) {
    p.prepareReparse();
    if (!p.run(src)) return false;
  }
  p.finish();
  return true;
}

}  // namespace ical
