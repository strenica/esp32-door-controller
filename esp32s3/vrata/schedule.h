// Expanded calendar occurrences for the relay decision.
//
// The original script ran every 5 minutes from cron, slept 10 s, then held the pin HIGH
// only for events already in progress, so an event started up to ~5 min
// late. Here the calendar is expanded into a small in-RAM list (rebuilt
// every 5 min and after each download) and the control task checks it every
// 250 ms, so the door opens within ~1 s of DTSTART.
//
// Bookings ("Name-PIN" titles) are kept in the same list: on doors with the
// PIN portal they only grant PIN access, they do not open the door.
#pragma once
#include <Arduino.h>

#include "ical.h"

namespace schedule {

enum class Source : uint8_t { None = 0, Local = 1, Remote = 2 };

struct Info {
  Source source = Source::None;
  bool ok = false;
  int64_t builtAt = 0;  // UTC
  uint32_t fileSize = 0;
  uint32_t events = 0;
  uint32_t occurrences = 0;
  uint32_t bookings = 0;
  uint32_t skipped = 0;  // all-day, unknown timezone, unsupported rules
  bool truncated = false;
  String error;
};

void begin();
// keepEndedS: also keep occurrences that ended this long ago (PIN "after" window).
bool rebuild(Source src, int64_t nowUtc, int64_t keepEndedS);
void clear();

// An event that holds the door open now. Bookings count only if !bookingsAreAccessOnly.
bool activeAt(Source src, int64_t nowUtc, bool bookingsAreAccessOnly, ical::Occurrence* which);
bool nextAfter(Source src, int64_t nowUtc, ical::Occurrence& out);
bool findBooking(Source src, const String& user, const String& pin, int64_t nowUtc, int64_t beforeS, int64_t afterS,
                 ical::Occurrence* out);
Info info();

const char* pathFor(Source src);
const char* sourceName(Source src);

}  // namespace schedule
