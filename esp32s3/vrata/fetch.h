// HTTP(S) transfers (replaces requests.get() and the wget in calendar.sh /
// remote_script.sh).
//
// Certificates are verified against the Mozilla root store compiled into
// the ESP32 core. Only one transfer runs at a time (a TLS session needs
// ~40 KB of RAM). Calendars are streamed to a temp file, validated and only
// then replace the old file, like app.py's temp_cal.ics -> cal.ics.
#pragma once
#include <Arduino.h>

namespace fetch {

struct Result {
  bool ok = false;
  int httpCode = 0;
  size_t bytes = 0;
  String error;
};

// Server API call: POST JSON, response body into `response`.
Result postJson(const String& url, const String& body, String& response, size_t maxBytes);

// Download an .ics file into `path` if it is a complete iCalendar file.
Result downloadCalendar(const String& url, const char* path);

// Firmware update from the server straight into the inactive app slot.
// Verifies size and MD5; the caller reboots on success.
Result downloadFirmware(const String& url, const String& mac, const String& token, const String& md5,
                        size_t size);

}  // namespace fetch
