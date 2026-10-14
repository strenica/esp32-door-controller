#include "selftest.h"

#include <string.h>

#include <vector>

#include "applog.h"
#include "auth.h"
#include "ical.h"
#include "tzconv.h"

namespace selftest {

namespace {

// Includes the first event of the original controller's test calendar plus cases for every
// feature the relay decision depends on. Expected values were computed by
// hand; CET = UTC+1, CEST = UTC+2 (DST ended 27.10.2024 at 01:00 UTC).
const char kCalendar[] =
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "X-WR-TIMEZONE:Europe/Belgrade\r\n"
    // original cal.ics: weekdays 14:45-16:00 Belgrade time
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Belgrade:20240822T144500\r\n"
    "DTEND;TZID=Europe/Belgrade:20240822T160000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=FR,MO,TH,TU,WE\r\n"
    "UID:selftest-weekday@example.com\r\n"
    "SUMMARY:14:45 - 15:00\r\n"
    "END:VEVENT\r\n"
    // daily with COUNT and an EXDATE, folded line
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Ljubljana:20241001T080000\r\n"
    "DTEND;TZID=Europe/Ljubljana:20241001T083000\r\n"
    "RRULE:FREQ=DAILY;COUNT=40\r\n"
    "EXDATE;TZID=Europe/Ljubljana:20241029T0800\r\n"
    " 00\r\n"
    "UID:daily@test\r\n"
    "SUMMARY:Daily\r\n"
    "END:VEVENT\r\n"
    // weekly Tuesday with one moved instance (RECURRENCE-ID)
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Ljubljana:20241001T100000\r\n"
    "DTEND;TZID=Europe/Ljubljana:20241001T110000\r\n"
    "RRULE:FREQ=WEEKLY\r\n"
    "UID:weekly@test\r\n"
    "SUMMARY:Weekly\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "RECURRENCE-ID;TZID=Europe/Ljubljana:20241029T100000\r\n"
    "DTSTART;TZID=Europe/Ljubljana:20241029T120000\r\n"
    "DTEND;TZID=Europe/Ljubljana:20241029T130000\r\n"
    "UID:weekly@test\r\n"
    "SUMMARY:Weekly (moved)\r\n"
    "END:VEVENT\r\n"
    // last Friday of the month
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Ljubljana:20240927T180000\r\n"
    "DTEND;TZID=Europe/Ljubljana:20240927T190000\r\n"
    "RRULE:FREQ=MONTHLY;BYDAY=-1FR\r\n"
    "UID:monthly@test\r\n"
    "SUMMARY:Last Friday\r\n"
    "END:VEVENT\r\n"
    // all-day (ignored by default) and cancelled events
    "BEGIN:VEVENT\r\n"
    "DTSTART;VALUE=DATE:20241030\r\n"
    "DTEND;VALUE=DATE:20241031\r\n"
    "UID:allday@test\r\n"
    "SUMMARY:Holiday\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20241031T150000Z\r\n"
    "DTEND:20241031T160000Z\r\n"
    "STATUS:CANCELLED\r\n"
    "UID:cancel@test\r\n"
    "SUMMARY:Cancelled\r\n"
    "END:VEVENT\r\n"
    // UTC single event, floating time, DURATION with a VALARM inside
    "BEGIN:VEVENT\r\n"
    "DTSTART:20241104T050800Z\r\n"
    "DTEND:20241104T051200Z\r\n"
    "UID:utc@test\r\n"
    "SUMMARY:UTC\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20241102T090000\r\n"
    "DTEND:20241102T100000\r\n"
    "UID:floating@test\r\n"
    "SUMMARY:Floating\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20241103T100000Z\r\n"
    "DURATION:PT1H30M\r\n"
    "UID:duration@test\r\n"
    "SUMMARY:Duration\r\n"
    "BEGIN:VALARM\r\n"
    "TRIGGER:-PT15M\r\n"
    "DURATION:PT5M\r\n"
    "REPEAT:1\r\n"
    "ACTION:DISPLAY\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    // gym booking: "Name-PIN" title, 18:00-19:00 CET = 17:00-18:00 UTC
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Europe/Ljubljana:20241105T180000\r\n"
    "DTEND;TZID=Europe/Ljubljana:20241105T190000\r\n"
    "UID:booking@test\r\n"
    "SUMMARY:Test Oseba-4321\r\n"
    "END:VEVENT\r\n"
    // TZID only defined by a VTIMEZONE further down (forces a second pass)
    "BEGIN:VEVENT\r\n"
    "DTSTART;TZID=Custom/Zone:20241101T200000\r\n"
    "DTEND;TZID=Custom/Zone:20241101T210000\r\n"
    "UID:vtz@test\r\n"
    "SUMMARY:Custom zone\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:Custom/Zone\r\n"
    "BEGIN:DAYLIGHT\r\n"
    "TZOFFSETFROM:+0100\r\n"
    "TZOFFSETTO:+0200\r\n"
    "DTSTART:19700329T020000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n"
    "END:DAYLIGHT\r\n"
    "BEGIN:STANDARD\r\n"
    "TZOFFSETFROM:+0200\r\n"
    "TZOFFSETTO:+0100\r\n"
    "DTSTART:19701025T030000\r\n"
    "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n"
    "END:VCALENDAR\r\n";

class MemSource : public ical::ByteSource {
 public:
  MemSource(const char* d, size_t n) : d_(d), n_(n) {}
  int read(uint8_t* buf, size_t len) override {
    const size_t k = (n_ - pos_) < len ? (n_ - pos_) : len;
    memcpy(buf, d_ + pos_, k);
    pos_ += k;
    return static_cast<int>(k);
  }
  bool rewind() override {
    pos_ = 0;
    return true;
  }

 private:
  const char* d_;
  size_t n_;
  size_t pos_ = 0;
};

struct Probe {
  int64_t t;
  bool expect;
  const char* what;
};

const Probe kProbes[] = {
    {1729860600, true, "weekly 14:50 CEST on Friday"},
    {1729864800, true, "weekly end is inclusive (16:00 CEST)"},
    {1729864801, false, "weekly after end"},
    {1729950600, false, "weekly not on Saturday"},
    {1730123400, true, "weekly 14:50 CET after DST change"},
    {1730185800, false, "daily excluded by EXDATE"},
    {1730272200, true, "daily next day"},
    {1731136200, true, "daily 40th instance"},
    {1731222600, false, "daily after COUNT"},
    {1730194200, false, "weekly moved away (RECURRENCE-ID)"},
    {1730201400, true, "weekly moved instance"},
    {1730799000, true, "weekly following week"},
    {1732901400, true, "monthly last Friday of November"},
    {1732296600, false, "monthly not on the other Fridays"},
    {1730289600, false, "all-day event ignored"},
    {1730388600, false, "cancelled event"},
    {1730697000, true, "UTC single event"},
    {1730697180, false, "UTC single event ended"},
    {1730536200, true, "floating time = local time"},
    {1730539800, false, "floating event ended"},
    {1730632500, true, "DURATION (not the VALARM's)"},
    {1730633460, false, "DURATION ended"},
    {1730489400, true, "TZID from a later VTIMEZONE"},
};

bool active(const std::vector<ical::Occurrence>& occ, int64_t t, bool skipBookings = false) {
  for (const ical::Occurrence& o : occ) {
    if (skipBookings && o.user[0]) continue;
    if (o.start <= t && t <= o.end) return true;
  }
  return false;
}

struct BookingProbe {
  const char* title;
  bool isBooking;
  const char* user;
  const char* pin;
};

const BookingProbe kBookingProbes[] = {
    {"James Alice-1234", true, "jamesalice", "1234"},
    {"Ana \xC5\xA0" "olar - 902211", true, "anasolar", "902211"},  // "Ana Šolar - 902211"
    {"Ana Novak-Kranjc-55667788", true, "ananovakkranjc", "55667788"},
    {"Seja 2024-25", false, "", ""},
    {"Trening U-12345678901", false, "", ""},
    {"Trening", false, "", ""},
    {"-1234", false, "", ""},
};

bool check(bool cond, const char* what, int& fails) {
  if (!cond) {
    LOGE("test", "FAIL: %s", what);
    ++fails;
  }
  return cond;
}

}  // namespace

bool run() {
  int fails = 0;

  // Timezone rules: EU DST switches at 01:00 UTC on the last Sundays.
  tz::Rule cet;
  check(tz::parsePosix("CET-1CEST,M3.5.0,M10.5.0/3", cet), "parse POSIX TZ", fails);
  check(tz::utcOffsetAt(cet, 1711846799) == 3600, "CET before spring change", fails);
  check(tz::utcOffsetAt(cet, 1711846800) == 7200, "CEST after spring change", fails);
  check(tz::utcOffsetAt(cet, 1729990799) == 7200, "CEST before autumn change", fails);
  check(tz::utcOffsetAt(cet, 1729990800) == 3600, "CET after autumn change", fails);
  check(tz::builtinPosix("/freeassociation.sourceforge.net/Europe/Ljubljana") != nullptr, "TZID prefix strip",
        fails);

  // Calendar expansion over a fixed window (Aug 2024 .. Jan 2025).
  MemSource src(kCalendar, sizeof(kCalendar) - 1);
  ical::Options opt;
  opt.windowStart = 1722470400;  // 2024-08-01
  opt.windowEnd = 1735689600;    // 2025-01-01
  opt.deviceTz = &cet;
  std::vector<ical::Occurrence> occ;
  ical::Stats st;
  if (check(ical::expand(src, opt, occ, st), "calendar parses", fails)) {
    for (const Probe& p : kProbes) check(active(occ, p.t) == p.expect, p.what, fails);
    check(st.reparsed, "second pass for late VTIMEZONE", fails);
    check(st.skippedAllDay == 1 && st.cancelled == 1, "all-day/cancelled counters", fails);

    // Bookings: PIN valid 15 min before 17:00Z until 15 min after 18:00Z.
    const int64_t win = 15 * 60;
    check(ical::findBooking(occ, "testoseba", "4321", 1730825400, win, win) != nullptr, "booking: 10 min early", fails);
    check(ical::findBooking(occ, "testoseba", "4321", 1730824800, win, win) == nullptr, "booking: 20 min early", fails);
    check(ical::findBooking(occ, "testoseba", "4321", 1730830200, win, win) != nullptr, "booking: 10 min after", fails);
    check(ical::findBooking(occ, "testoseba", "4321", 1730830800, win, win) == nullptr, "booking: 20 min after", fails);
    check(ical::findBooking(occ, "testoseba", "4322", 1730827800, win, win) == nullptr, "booking: wrong PIN", fails);
    check(ical::findBooking(occ, "testosebe", "4321", 1730827800, win, win) == nullptr, "booking: wrong user", fails);
    check(active(occ, 1730827800, false), "booking opens a door without portal", fails);
    check(!active(occ, 1730827800, true), "booking does not open a portal door", fails);
    for (const ical::Occurrence& o : occ) {
      if (o.user[0]) check(strstr(o.summary, "4321") == nullptr, "PIN hidden in summary", fails);
    }
  }

  for (const BookingProbe& b : kBookingProbes) {
    std::string name, user, pin;
    const bool isB = ical::parseBooking(b.title, name, user, pin);
    check(isB == b.isBooking && (!isB || (user == b.user && pin == b.pin)), b.title, fails);
  }

  check(auth::selfTest(), "PBKDF2-HMAC-SHA256 test vector", fails);

  if (fails == 0) {
    LOGI("test", "self-test passed (%u occurrences checked)", static_cast<unsigned>(occ.size()));
  } else {
    LOGE("test", "SELF-TEST FAILED: %d check(s) - schedule results may be wrong", fails);
  }
  return fails == 0;
}

}  // namespace selftest
