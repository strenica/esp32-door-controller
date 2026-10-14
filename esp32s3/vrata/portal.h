// PIN portal (from Esp1/door_firmware_portal), switched on
// per door on the server.
//
// Phones join the open Wi-Fi of the door, a sign-in page pops up, and a
// username + PIN opens the relay for the configured time. Valid PINs:
//   - permanent users managed on the server (valid any time), and
//   - calendar bookings "James Alice-123456" (valid from 15 min before the
//     event until 15 min after it, both adjustable on the server).
// Status LOCK (0) blocks every PIN. Phones are disconnected after unlocking,
// when idle, and refused for 30 s afterwards; guessing is throttled per
// phone and per account.
//
// Differences from the stand-alone gym firmware: users and the admin
// password moved to the server (no admin page or serial commands on the open
// network), and the door also follows its calendar/status like every door.
//
// This module also owns port 80: on the school network and on the setup
// network it redirects to the control panel on port 5000; on the portal
// network it serves the sign-in page, and port 5000 is refused there.
#pragma once
#include <Arduino.h>

namespace portal {

struct Info {
  bool enabled = false;
  bool apUp = false;
  String ssid;
  int phones = 0;
  uint32_t unlockLeftS = 0;
  uint32_t pausedLeftS = 0;  // network switched off after an unlock
  int users = 0;
};

void begin();        // port-80 server
void reconfigure();  // reload users and times from settings
void tick();         // ~1 Hz from the control task: phone management
bool enabled();      // portal switched on for this door
bool unlocked();     // a valid PIN opened the door and the time is not over
void lockNow();      // also ends a network pause
// After a successful unlock the portal network is switched off (after the
// "Unlocked" message had time to show) for the configured pause, so phones and
// tablets stop reconnecting while the door is open anyway. Default: unlock
// time - 60 s, i.e. the network is back one minute before the door locks.
bool apPaused();
bool isPortalConnection(const IPAddress& localIp);
Info info();

}  // namespace portal
