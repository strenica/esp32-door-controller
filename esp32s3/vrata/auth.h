// Login, password storage and sessions.
//
// Passwords are PBKDF2-HMAC-SHA256 (hardware SHA), salted per password and
// compared in constant time. Sessions are random 128-bit cookies kept in RAM
// (lost on reboot), each with its own CSRF token and pending "flash" messages.
#pragma once
#include <Arduino.h>

#include <vector>

namespace auth {

struct Flash {
  String category;  // Bootstrap alert class: success, warning, danger, info
  String text;
};

struct Session {
  bool used = false;
  char token[33] = {0};
  char csrf[33] = {0};
  uint32_t lastSeenMs = 0;
  std::vector<Flash> flashes;
};

void begin();  // creates the default password record on first boot

bool checkLogin(const String& user, const String& pass);
bool checkPassword(const String& pass);
bool setPassword(const String& pass);
void resetPasswordToDefault();  // BOOT button

// Session handling (web task only, except endAllSessions()).
Session* sessionFromCookie(const String& cookieHeader);
Session* createSession();
void endSession(Session* s);
void endAllSessions();  // safe from any task

// Brute-force protection: after LOGIN_MAX_FAILURES the page locks out.
bool lockedOut(uint32_t& secondsLeft);
void noteFailure();
void noteSuccess();

bool selfTest();  // PBKDF2 test vector

}  // namespace auth
