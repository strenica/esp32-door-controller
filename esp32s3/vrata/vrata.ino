// ============================================================================
//  VRATA - calendar-driven door controller for the ESP32-S3
//
//  The door relay opens while an event of the door's calendar is in progress.
//  The server (DEFAULT_SERVER_URL in config.h) decides per door whether it is locked (0),
//  open (1), follows its group calendar (2) or its own calendar (3), and can
//  enable the PIN portal (gym door): phones join the door's open Wi-Fi and
//  unlock it with a name + PIN (permanent users or calendar bookings).
//  A local page on port 5000 shows the status and allows changes while the
//  server is unreachable.
//
//  Board:   ESP32S3 Dev Module (esp32 core 3.x), see the root README.md
//  Config:  config.h (pins, defaults, intervals), secrets.h (private values)
//
//  Task layout:
//    control (prio 4)  relay decision every 250 ms, BOOT button, portal phones
//    net     (prio 2)  Wi-Fi, NTP, server sync, calendar downloads/parsing, OTA
//    web     (prio 2)  control panel (port 5000), captive DNS
//    httpd   (prio 3)  port 80: PIN portal / redirect to the control panel
//  control, net and web are subscribed to the task watchdog.
// ============================================================================
#include <Arduino.h>

#include "applog.h"
#include "auth.h"
#include "cloud.h"
#include "config.h"
#include "controller.h"
#include "netmgr.h"
#include "portal.h"
#include "relay.h"
#include "schedule.h"
#include "selftest.h"
#include "settings.h"
#include "storage.h"
#include "tzconv.h"
#include "wdt.h"
#include "webui.h"

void setup() {
  relay::begin();  // first thing: door locked while everything else starts
  Serial.begin(115200);
  delay(200);
  applog::begin();
  LOGI("main", "%s %s starting (reset: %s)", FW_NAME, FW_VERSION, cloud::resetReasonText());

  wdt::init(WDT_TIMEOUT_S);
  tz::setDeviceRule(DEVICE_TZ_POSIX);
  settings::begin();
  cloud::bootCheck();  // firmware rollback bookkeeping, door MAC, pairing code
  storage::begin();
  schedule::begin();
  auth::begin();
#if RUN_SELFTEST_AT_BOOT
  selftest::run();
#endif
  netmgr::begin();
  portal::begin();
  ctl::begin();
  webui::begin();
  LOGI("main", "running, free heap %u KB", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

void loop() {
  // Everything runs in FreeRTOS tasks; the Arduino loop task is not needed.
  vTaskDelete(nullptr);
}
