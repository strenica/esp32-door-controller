// Persistent settings in NVS (flash key/value store).
//
// Replaces the Pi's state files (calendar.sh, remote.txt, status.cfg,
// user_data.txt, crontab ...). Most values now come from the server and are
// cached here, so the door keeps working with its last settings when the
// server or the internet is down.
#pragma once
#include <Arduino.h>

#include <functional>

#include "config.h"

namespace settings {

struct WifiNet {
  String ssid;
  String pass;
};

struct Data {
  // identity / network
  String deviceName;
  WifiNet wifi[WIFI_MAX_NETWORKS];
  WifiNet wifiBackup[WIFI_MAX_NETWORKS];  // previous networks during a trial
  bool wifiTrial = false;                 // server Wi-Fi applied, not yet proven
  uint32_t wifiRev = 0;                   // server Wi-Fi revision applied
  uint32_t wifiRejectedRev = 0;           // revision that failed to connect
  uint8_t setupAp = 0;                    // 0 = automatic, 1 = BOOT button only

  // server link
  String serverUrl;
  String token;                           // door key, empty until adopted
  String pairCode;                        // shown locally, compared on adoption
  uint32_t configRev = 0;
  uint16_t pollS = DEFAULT_POLL_S;
  uint16_t calS = DEFAULT_CALENDAR_S;

  // schedule
  uint8_t mode = 0;                       // ctl::Mode
  int8_t serverStatus = -1;               // last status from the server (0..3)
  String localUrl;                        // local calendar (status 3)
  String remoteCalUrl;                    // group calendar (status 2)

  // PIN portal
  bool portalEnabled = false;
  String portalSsid;
  uint16_t portalUnlockS = 600;
  uint16_t portalPauseS = 540;            // portal network off after an unlock (0 = never)
  uint16_t pinBeforeMin = 15;
  uint16_t pinAfterMin = 15;
  String portalUsers;                     // "user:pin\n" lines

  // local admin + firmware updates
  String pwRecord;
  String otaTry;                          // version being installed
  String otaBad;                          // version that failed (not retried)
  uint8_t otaSrc = 0;                     // 1 = server, 2 = local upload
};

void begin();
Data get();  // copy, safe from any task

// Change settings atomically: modify([](settings::Data& d) { d.localUrl = x; });
// Only the fields that really changed are written to flash.
void modify(const std::function<void(Data&)>& fn);

void factoryReset();  // wipe the namespace; defaults apply after reboot

}  // namespace settings
