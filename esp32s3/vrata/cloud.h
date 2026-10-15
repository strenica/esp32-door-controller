// Link to the management server (DEFAULT_SERVER_URL in config.h/secrets.h).
//
// Every poll interval the door POSTs its state to <server>/api/door.php and
// gets its full configuration back (status, calendars, intervals, portal,
// Wi-Fi, assigned firmware). The server is the single source of settings,
// calendars and the event log.
//
//  - New door: sends its MAC + pairing code and shows up on the server as
//    "pending"; after an admin adopts it, the next reply carries its key.
//  - Server unreachable for OFFLINE_AFTER_FAILURES polls: the door keeps its
//    last settings and its local page becomes editable ("offline").
//  - Firmware from the server: installed into the spare app slot; the new
//    firmware must reach the server within 10 min or the board rolls back.
#pragma once
#include <Arduino.h>

namespace cloud {

struct Info {
  String serverUrl;
  String state;      // human readable, e.g. "connected", "waiting for adoption"
  bool adopted = false;
  bool managed = false;
  String pairCode;
  int64_t lastOkAt = 0;
  String lastError;
  bool otaPendingVerify = false;
  String otaError;
};

void bootCheck();  // early in setup(): rollback bookkeeping, boot id
void tick();       // net task: sync when due
void requestSync();

bool adopted();
bool managed();    // adopted and the server answered recently

// Queue an event for the server log (unlocks, mode changes, offline edits).
void logEvent(const char* type, const String& msg);

void noteLocalOta();  // /update page: confirm the new image after 3 min
Info info();
const char* resetReasonText();

}  // namespace cloud
