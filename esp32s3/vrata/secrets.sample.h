// ============================================================================
//  VRATA - private device defaults (TEMPLATE)
//
//  Copy this file to secrets.h (same folder) and fill in your own values.
//  secrets.h is listed in .gitignore - never commit it. Every value here is
//  optional: anything you leave out falls back to the generic default in
//  config.h, so the sketch compiles with or without secrets.h.
// ============================================================================
#pragma once

// Address of the management server, without a trailing slash (e.g.
// "https://doors.example.com/oss"). The door asks it for its settings and
// reports its state. Use a host with a valid HTTPS certificate - the door
// refuses servers whose certificate it cannot verify (TLS_VERIFY in config.h).
#define DEFAULT_SERVER_URL          "https://example.com/oss"

// Password of the door's LOCAL page (http://<ip>:5000), user "admin".
// Change it from the default before flashing. At least 8 characters.
#define DEFAULT_ADMIN_PASSWORD      "change-me-please"

// Optional Wi-Fi networks the door tries at boot (up to three, tried by signal
// strength). They can also be entered through the setup network, or set from
// the server later. Leave empty to start in setup mode.
#define DEFAULT_WIFI1_SSID          ""
#define DEFAULT_WIFI1_PASSWORD      ""
#define DEFAULT_WIFI2_SSID          ""
#define DEFAULT_WIFI2_PASSWORD      ""
#define DEFAULT_WIFI3_SSID          ""
#define DEFAULT_WIFI3_PASSWORD      ""

// Private iCal URL for status 3 (the door's own calendar). Normally left empty
// here and set per door on the server.
#define DEFAULT_LOCAL_CALENDAR_URL  ""

// Password of the setup network (VRATA-SETUP-xxxx), at least 8 characters.
// The default in config.h is public, so set your own before flashing.
#define AP_PASSWORD                 "change-me-too"
