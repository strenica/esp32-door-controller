// Streaming iCalendar (RFC 5545) parser + recurrence expander.
//
// Replaces the Python packages icalendar + recurring_ical_events used by
// ical.py. The file is read line by line (RAM use does not depend on the file
// size) and only occurrences overlapping [windowStart, windowEnd] are kept,
// exactly like recurring_ical_events.of(cal).between(now-2d, now+10d).
//
// Supported: VEVENT with DTSTART/DTEND/DURATION (UTC, TZID or floating),
// RRULE FREQ=DAILY|WEEKLY|MONTHLY|YEARLY with INTERVAL, COUNT, UNTIL, BYDAY
// (incl. ordinals like 2MO / -1FR), BYMONTHDAY, BYMONTH, BYSETPOS, WKST;
// EXDATE, RDATE, RECURRENCE-ID overrides, STATUS:CANCELLED, VTIMEZONE (as a
// fallback for TZIDs missing from the built-in table), line folding.
// Not supported (rule ignored, only DTSTART is used, a warning is logged):
// FREQ=HOURLY/MINUTELY/SECONDLY, BYWEEKNO, BYYEARDAY, BYHOUR/BYMINUTE/BYSECOND.
//
// Pure C++ (no Arduino headers) so it can be unit-tested on a PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "tzconv.h"

namespace ical {

struct Occurrence {
  int64_t start;     // UTC epoch seconds
  int64_t end;       // UTC epoch seconds; active while start <= now <= end
  uint32_t uid;      // FNV-1a hash of the UID
  uint8_t flags;     // internal (override marker)
  char summary[40];  // UTF-8 display text, truncated; a booking's PIN is masked
  char user[21];     // booking: normalized username ("" = ordinary event)
  char pin[9];       // booking: 4-8 digit PIN
};

class ByteSource {
 public:
  virtual ~ByteSource() {}
  virtual int read(uint8_t* buf, size_t len) = 0;  // <= 0 means end of data
  virtual bool rewind() = 0;
};

struct Options {
  int64_t windowStart = 0;
  int64_t windowEnd = 0;
  int64_t dropEndedBefore = 0;       // occurrences that ended earlier are useless
  const tz::Rule* deviceTz = nullptr;  // floating times + all-day events
  bool allDayActivates = false;
  int32_t defaultDurationS = 3600;
  size_t maxOccurrences = 600;
  void (*yield)() = nullptr;         // called regularly (watchdog / scheduler)
  void (*warn)(const char* msg) = nullptr;
};

struct Stats {
  uint32_t lines = 0;
  uint32_t events = 0;
  uint32_t recurring = 0;
  uint32_t overrides = 0;
  uint32_t cancelled = 0;
  uint32_t skippedAllDay = 0;
  uint32_t skippedTz = 0;
  uint32_t unsupportedRules = 0;
  uint32_t occurrences = 0;
  bool validHeader = false;  // first line was BEGIN:VCALENDAR
  bool truncated = false;    // more than maxOccurrences in the window
  bool reparsed = false;     // a VTIMEZONE appeared after its first use
};

// Parses the calendar and fills `out` (sorted by start). Returns false only
// if the data is not an iCalendar stream at all.
bool expand(ByteSource& src, const Options& opt, std::vector<Occurrence>& out, Stats& stats);

// Exposed for tests.
uint32_t fnv1a(const std::string& s);

// Username from a name: lowercase letters and digits only, č/š/ž/ć/đ and
// accented Latin letters simplified (Ana Šolar -> anasolar), max 20 characters.
// MUST match normalize_username() in server/oss/lib/doors.php.
std::string normalizeUser(const std::string& s);

// Booking event title "James Alice-123456" (also "James Alice - 123456"):
// the text after the LAST hyphen is a 4-8 digit PIN, the part before it the
// person's name. Returns false for ordinary titles.
bool parseBooking(const std::string& summary, std::string& name, std::string& user, std::string& pin);

// Booking for user+PIN that is valid at `now` (from start - before to end + after).
const Occurrence* findBooking(const std::vector<Occurrence>& occ, const std::string& user, const std::string& pin,
                              int64_t now, int64_t beforeS, int64_t afterS);

// Constant-time string compare (PINs).
bool safeEquals(const char* a, const char* b);

}  // namespace ical
