// Wi-Fi, the door's own network, NTP and mDNS (replaces the OS network
// stack, NetworkManager's saved networks and systemd-timesyncd).
//
// - Up to WIFI_MAX_NETWORKS saved networks; the strongest visible one is used.
//   The server may replace them; if the new ones do not connect within
//   WIFI_TRIAL_MS the door goes back to the previous ones.
// - The ESP32 can run ONE network of its own, used for either:
//     setup  "VRATA-SETUP-xxxx" (WPA2): Wi-Fi not configured, offline for
//            60 s (if allowed), or 10 min after a short BOOT press;
//     portal open network of the PIN portal (gym door), when enabled.
//   The setup network wins while it is requested; the portal pauses.
// - SNTP keeps the clock in sync (tries 193.2.4.6 first, like the Pis).
// - mDNS: http://<device-name>.local:5000/
#pragma once
#include <Arduino.h>

#include "settings.h"

namespace netmgr {

struct Info {
  bool staConnected = false;
  String ssid;
  String ip;
  int rssi = 0;
  bool apActive = false;
  String apMode;  // "", "setup" or "portal"
  String apSsid;
  String apIp;
  String hostname;
  bool wifiTrial = false;
};

void begin();
void tick();               // net task, ~every 500 ms
void requestReconfigure();  // after Wi-Fi, name, portal changes (any task)
void processDns();         // web task: captive-portal DNS while our network runs
void openSetupAp();        // short BOOT press
void applyServerWifi(const settings::WifiNet* nets, int count, uint32_t rev);

bool online();
bool timeValid();
bool portalApActive();
IPAddress portalIp();
Info info();

String hostnameFor(const String& deviceName);

}  // namespace netmgr
