// Login, password storage and sessions (replaces Flask-Login + Werkzeug).
//
// Werkzeug stored scrypt(N=32768, r=8) hashes, which need 32 MB of RAM to
// verify, so the port uses PBKDF2-HMAC-SHA256 (hardware SHA) instead. The
// old user_data.txt hash therefore cannot be carried over.
// Sessions are random 128-bit cookies kept in RAM (lost on reboot), each with
// its own CSRF token and pending "flash" messages.
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

// Brute-force protection (new; Flask had none).
bool lockedOut(uint32_t& secondsLeft);
void noteFailure();
void noteSuccess();

bool selfTest();  // PBKDF2 test vector

}  // namespace auth
