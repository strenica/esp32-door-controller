// Operating modes and background jobs (replaces cron, the lock file,
// ical.py, icalNOTIME.py, calendar.sh, remote_script.sh and remote_ical.py).
//
// The mode comes from the server's status for this door:
//   0 LOCK -> Stopped, 1 OPEN -> ManualOn, 2 SCHEDULE REMOTE -> RemoteSchedule
//   (group calendar), 3 SCHEDULE LOCAL -> LocalSchedule (the door's own calendar).
// While the server is unreachable, the local web page may change the mode.
//
// The relay is ON (door open) when:
//   - the server's "unlock now" time has not passed (works in every mode), or
//   - mode is ManualOn, or
//   - mode is a schedule and an event is running (on PIN-portal doors,
//     "Name-PIN" booking events only grant PIN access), or a valid PIN was
//     entered on the portal (not in Stopped/LOCK).
#pragma once
#include <Arduino.h>

#include <vector>

#include "auth.h"
#include "schedule.h"

namespace ctl {

enum class Mode : uint8_t { Stopped = 0, ManualOn = 1, LocalSchedule = 2, RemoteSchedule = 3 };

using Flashes = std::vector<auth::Flash>;

struct Status {
  Mode mode = Mode::Stopped;
  int serverStatus = -1;
  bool relayOn = false;
  String openReason;
  int64_t openUntil = 0;
  bool timeValid = false;
  int64_t nowUtc = 0;
  bool eventActive = false;
  String eventSummary;
  int64_t eventEnd = 0;
  bool hasNext = false;
  String nextSummary;
  int64_t nextStart = 0;
  schedule::Info sched;
  String lastDownload;  // result of the last calendar download
  int64_t lastDownloadAt = 0;
};

void begin();  // restores the mode, starts the control + network tasks

// From the server
void applyServerStatus(int status);
void setOpenUntil(int64_t utc);
void requestCalendarRefresh();

// For the PIN portal
bool pinsBlocked();                  // LOCK: no PIN opens the door
schedule::Source bookingSource();    // calendar that holds the bookings

// Local web actions (named after the buttons of the original app.py).
// While the server manages the door they only report that.
void actionStart(Flashes& f);
void actionStop(Flashes& f);
void actionLocalSchedule(Flashes& f);
void actionRemoteSchedule(Flashes& f);
void actionUpdateLocalUrl(const String& url, Flashes& f);
void actionSync(Flashes& f);
void actionSaveSettings(const String& deviceName, const String ssid[], const String pass[], Flashes& f);
void actionServerUrl(const String& url, Flashes& f);

Status status();
const char* modeLabel(Mode m);        // the badge text of the original UI
const char* statusName(int s);        // LOCK / OPEN / SCHEDULE REMOTE / ...
String formatTime(int64_t utc);       // "dd.mm.yyyy hh:mm" local time

}  // namespace ctl
