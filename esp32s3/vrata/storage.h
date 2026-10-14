// LittleFS storage for the downloaded calendars (was /home/admin/ical_event).
//
// The Pi kept ONE cal.ics that both calendar.sh (local) and the remote
// download overwrote, so in status 3 the two fought over the same file.
// The port keeps one file per source and evaluates the one that matches the
// current mode.
#pragma once
#include <Arduino.h>

namespace storage {

extern const char* const kLocalIcs;   // Local Schedule calendar
extern const char* const kRemoteIcs;  // Remote Schedule calendar

bool begin();  // mounts (and on first use formats) the data partition
bool mounted();
size_t freeBytes();
void wipe();  // factory reset: delete the calendars

// Held while a calendar file is replaced or parsed.
void lock();
void unlock();
struct Lock {
  Lock() { lock(); }
  ~Lock() { unlock(); }
};

}  // namespace storage
