// LittleFS storage for the downloaded calendars.
//
// One file per source (the door's own calendar and the group calendar), so
// they can never overwrite each other; the relay decision reads whichever one
// matches the current mode.
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
