// ============================================================================
//  VRATA - ESP32-S3 calendar-driven door controller.
//  Central configuration: every pin and tunable lives here.
//  Requires the "esp32 by Espressif Systems" Arduino core 3.x (tested 3.3.12).
// ============================================================================
//
//  WIRING (ESP32-S3-DevKitC-1 or any ESP32-S3 module)
//  ---------------------------------------------------------------------------
//  ESP32-S3 pin   Relay board / part
//  ---------------------------------------------------------------------------
//  5V (VBUS)      relay board VCC (red)
//  GND            relay board GND (black)
//  GPIO4          relay board IN  (purple)
//  GPIO0          on-board BOOT button (setup network, resets)
//  (GPIO2)        optional status LED (disabled by default)
//  ---------------------------------------------------------------------------
//  The relay input is active-HIGH: 3.3 V on GPIO4 closes the contact that
//  opens the door. A 1-channel relay board switching from 3.3 V logic is
//  what this expects (RELAY_ACTIVE_HIGH below).
//  Recommended: 10 kOhm GPIO4 -> GND so the relay stays OFF (door locked)
//  while the ESP32 is in reset and the pin floats.
//
//  Pins deliberately avoided:
//    GPIO0/3/45/46  strapping pins (GPIO0 is only read as a button at runtime)
//    GPIO19/20      native USB D-/D+
//    GPIO26..32     SPI flash / PSRAM
//    GPIO33..37     octal PSRAM on *R8 modules
//    GPIO43/44      UART0 TX/RX (serial log)
//    GPIO38/48      RGB LED on DevKitC-1 v1.0 / v1.1
//
//  The RPi original also drove GPIO12 LOW on "Stop". Nothing ever drove it
//  HIGH, so it was a no-op/typo; the port has only the relay output.
// ============================================================================
#pragma once

// Private, per-device values (server URL, local password, Wi-Fi networks) live
// in secrets.h. Copy secrets.sample.h to secrets.h and fill it in; secrets.h is
// not committed. Without it the generic defaults further down are used, so the
// sketch always compiles.
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#define FW_NAME     "VRATA-ESP32S3"
#define FW_VERSION  "2.1.2"   // the server reads this from the .bin on upload

// ---------------------------------------------------------------- pins -----
#define PIN_RELAY               4
#define RELAY_ACTIVE_HIGH       1     // 1 = HIGH on GPIO4 opens the door
#define PIN_BUTTON              0     // BOOT button, active LOW
#define PIN_STATUS_LED          -1    // e.g. 2 for an LED on GPIO2; -1 = none
#define STATUS_LED_ACTIVE_HIGH  1     // LED mirrors the relay state

// ------------------------------------------------------------- server -----
// The door asks this server for its settings every poll interval and reports
// its state (see README.md). Changeable in the door's web page.
// Override in secrets.h with your own server.
#ifndef DEFAULT_SERVER_URL
#define DEFAULT_SERVER_URL          "https://example.com/oss"
#endif
// A door that has not reached the server this many times in a row counts as
// offline: its local web page becomes editable until the server is back.
#define OFFLINE_AFTER_FAILURES      3
#define DEFAULT_POLL_S              60      // server may set 30..3600
#define DEFAULT_CALENDAR_S          300     // server may set 60..3600
// After a firmware update from the server, the new firmware must reach the
// server within this time, otherwise the board returns to the old firmware.
#define OTA_CONFIRM_TIMEOUT_MS      (10UL * 60UL * 1000UL)
#define OTA_LOCAL_CONFIRM_MS        (3UL * 60UL * 1000UL)    // /update uploads
// New Wi-Fi settings from the server are kept only if the board connects
// within this time; otherwise it goes back to the previous networks.
#define WIFI_TRIAL_MS               (5UL * 60UL * 1000UL)

// ------------------------------------------------- identity & defaults -----
// The device name is assigned on the server when the door is adopted
// (V1, V2 ...). Until then this name is used.
#define DEFAULT_DEVICE_NAME         "vrata-esp32"
#define ADMIN_USERNAME              "admin"
// Local web page password (http://<ip>:5000), user "admin". Override in
// secrets.h and change it before flashing; the default is a placeholder.
#ifndef DEFAULT_ADMIN_PASSWORD
#define DEFAULT_ADMIN_PASSWORD      "change-me-please"
#endif
// Private iCal URL for "Local Schedule" (normally set on the server).
#ifndef DEFAULT_LOCAL_CALENDAR_URL
#define DEFAULT_LOCAL_CALENDAR_URL  ""
#endif

// Up to three Wi-Fi networks, tried by signal strength. Can also be set on
// the server or in the door's web page.
// SSIDs with non-ASCII letters (e.g. "Šola") work; keep this file UTF-8.
// Override any of these in secrets.h.
#define WIFI_MAX_NETWORKS           3
#ifndef DEFAULT_WIFI1_SSID
#define DEFAULT_WIFI1_SSID          ""
#endif
#ifndef DEFAULT_WIFI1_PASSWORD
#define DEFAULT_WIFI1_PASSWORD      ""
#endif
#ifndef DEFAULT_WIFI2_SSID
#define DEFAULT_WIFI2_SSID          ""
#endif
#ifndef DEFAULT_WIFI2_PASSWORD
#define DEFAULT_WIFI2_PASSWORD      ""
#endif
#ifndef DEFAULT_WIFI3_SSID
#define DEFAULT_WIFI3_SSID          ""
#endif
#ifndef DEFAULT_WIFI3_PASSWORD
#define DEFAULT_WIFI3_PASSWORD      ""
#endif

// ------------------------------------------------------------- network -----
#define WEB_PORT                    5000   // local control panel
#define AP_SSID_PREFIX              "VRATA-SETUP-"
// WPA2, at least 8 characters. The default below is public, so anyone in
// Wi-Fi range could join a door's setup network while it is being put into
// service. Override it in secrets.h.
#ifndef AP_PASSWORD
#define AP_PASSWORD                 "vrata-setup"
#endif
#define AP_FALLBACK_AFTER_MS        60000UL  // no Wi-Fi this long -> setup AP
#define AP_STOP_AFTER_CONNECTED_MS  30000UL  // Wi-Fi back this long -> stop it
#define SETUP_AP_ON_DEMAND_MS       (10UL * 60UL * 1000UL)  // short BOOT press
#define WIFI_RETRY_INTERVAL_MS      30000UL
#define WIFI_RETRY_INTERVAL_AP_MS   120000UL // slower while phones use an AP
#define WIFI_CONNECT_TIMEOUT_MS     10000UL

// ------------------------------------------------- PIN portal (gym door) -----
// Open Wi-Fi + sign-in page that unlocks the door with username + PIN
// (ported from the standalone gym-door firmware). Switched on per door on the
// server.
#define PORTAL_DEFAULT_SSID         "Vrata"
#define PORTAL_MAX_CLIENTS          4
#define PORTAL_KICK_DELAY_MS        4000     // show "Unlocked", then disconnect
#define PORTAL_COOLDOWN_MS          30000    // a disconnected phone is refused
#define PORTAL_IDLE_KICK_MS         50000    // joined but did not sign in
#define PORTAL_MAX_FAILS            5        // then 60 s per phone, 1-8 min per account
#define PORTAL_LOCKOUT_MS           60000
#define PORTAL_FAIL_DELAY_MS        1000
#define PORTAL_MAX_USERS            100      // permanent users from the server

// ---------------------------------------------------------------- time -----
#define DEVICE_TZ_POSIX   "CET-1CEST,M3.5.0,M10.5.0/3"   // Europe/Ljubljana
// 193.2.4.6 (Arnes) was configured in timesyncd.conf on the Pis; school
// networks often block other NTP servers, so it is tried first.
#define NTP_SERVER_1      "193.2.4.6"
#define NTP_SERVER_2      "pool.ntp.org"
#define NTP_SERVER_3      "time.google.com"

// ---------------------------------------------------------- scheduling -----
#define SCHEDULE_REBUILD_INTERVAL_MS  (5UL * 60UL * 1000UL)
#define NET_RETRY_AFTER_FAILURE_MS    (60UL * 1000UL)
#define CONTROL_LOOP_MS               250UL    // relay + button polling
// Look-ahead window: now - 2 days .. now + 10 days
#define WINDOW_PAST_S                 (2L * 86400L)
#define WINDOW_FUTURE_S               (10L * 86400L)
// An event with no DTEND runs for one hour.
#define DEFAULT_EVENT_DURATION_S      3600
// All-day events have no time of day, so they cannot be scheduled. They are
// ignored by default; 1 = keep the door open for the whole day instead.
#define ALLDAY_EVENTS_ACTIVATE        0
#define MAX_OCCURRENCES               400
// Manual ON is not restored after a reboot - a door should not come back
// open by itself. 1 = restore the previous manual state.
#define RESTORE_MANUAL_ON_AT_BOOT     0

// ---------------------------------------------------------------- HTTP -----
#define HTTP_CONNECT_TIMEOUT_MS  10000
#define HTTP_TIMEOUT_MS          15000
#define TLS_HANDSHAKE_TIMEOUT_S  15
#define HTTP_MAX_REDIRECTS       5
#define MAX_ICS_BYTES            (1024UL * 1024UL)
#define MAX_API_BYTES            (32UL * 1024UL)
// 1 = verify HTTPS certificates against the Mozilla root store built into
// the ESP32 core.
// 0 = accept any certificate: never in production, because a fake server
// could then open the doors.
#define TLS_VERIFY               1

// ------------------------------------------------------------ security -----
#define PBKDF2_ITERATIONS          10000
#define SESSION_IDLE_TIMEOUT_S     (12UL * 3600UL)
#define MAX_SESSIONS               4
#define LOGIN_MAX_FAILURES         5
#define LOGIN_LOCKOUT_S            60
// Local firmware upload page (/update), in addition to updates from the server.
#define ENABLE_WEB_OTA             1

// -------------------------------------------------------------- button -----
#define BUTTON_SHORT_PRESS_MAX_MS      1500UL   // short press -> setup network 10 min
#define BUTTON_PASSWORD_RESET_HOLD_MS  10000UL  // -> default admin password
#define BUTTON_FACTORY_RESET_HOLD_MS   30000UL  // -> erase everything, reboot

// ---------------------------------------------------- tasks & watchdog -----
#define WDT_TIMEOUT_S         60     // longer than any blocking HTTP step
#define CONTROL_TASK_STACK    6144   // incl. PBKDF2 on a button reset
#define NET_TASK_STACK        12288  // TLS handshake + calendar parsing + JSON
#define WEB_TASK_STACK        12288  // web actions download calendars too
#define PORTAL_TASK_STACK     8192   // ESP-IDF http server for port 80
#define CONTROL_TASK_PRIO     4      // relay timing wins over everything
#define NET_TASK_PRIO         2
#define WEB_TASK_PRIO         2
#define APP_CPU               1      // Wi-Fi/LwIP run on core 0

// ------------------------------------------------------------- storage -----
// LittleFS is mounted on the first data partition found with one of these
// labels, so the stock Arduino IDE partition schemes work unchanged.
#define FS_PARTITION_LABELS   { "spiffs", "littlefs", "ffat" }

// ------------------------------------------------------------- logging -----
#define LOG_RING_BYTES        8192   // recent log lines shown at /log
#define RUN_SELFTEST_AT_BOOT  1      // parser/timezone/PBKDF2 self-test
