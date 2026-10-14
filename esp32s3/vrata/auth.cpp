#include "auth.h"

#include <esp_random.h>
#include <mbedtls/md.h>

#include "applog.h"
#include "config.h"
#include "settings.h"
#include "wdt.h"

namespace auth {

namespace {

Session s_sessions[MAX_SESSIONS];
volatile bool s_clearAll = false;
uint8_t s_failures = 0;
uint32_t s_lockUntilMs = 0;

// PBKDF2-HMAC-SHA256 on top of the mbedTLS HMAC API (stable across
// mbedTLS 2.x/3.x, unlike mbedtls_pkcs5_pbkdf2_hmac*).
bool pbkdf2(const uint8_t* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, uint32_t iterations,
            uint8_t* out, size_t outLen) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (!info || mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }
  uint8_t u[32], t[32];
  uint32_t block = 1;
  size_t done = 0;
  while (done < outLen) {
    const uint8_t counter[4] = {static_cast<uint8_t>(block >> 24), static_cast<uint8_t>(block >> 16),
                                static_cast<uint8_t>(block >> 8), static_cast<uint8_t>(block)};
    mbedtls_md_hmac_starts(&ctx, pw, pwLen);
    mbedtls_md_hmac_update(&ctx, salt, saltLen);
    mbedtls_md_hmac_update(&ctx, counter, 4);
    mbedtls_md_hmac_finish(&ctx, u);
    memcpy(t, u, sizeof(t));
    for (uint32_t i = 1; i < iterations; ++i) {
      mbedtls_md_hmac_reset(&ctx);
      mbedtls_md_hmac_update(&ctx, u, sizeof(u));
      mbedtls_md_hmac_finish(&ctx, u);
      for (int j = 0; j < 32; ++j) t[j] ^= u[j];
      if ((i & 2047) == 0) wdt::feed();
    }
    const size_t n = (outLen - done) < 32 ? (outLen - done) : 32;
    memcpy(out + done, t, n);
    done += n;
    ++block;
  }
  mbedtls_md_free(&ctx);
  return true;
}

String toHex(const uint8_t* b, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += kHex[b[i] >> 4];
    s += kHex[b[i] & 15];
  }
  return s;
}

bool fromHex(const String& s, uint8_t* out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    char buf[3] = {s[2 * i], s[2 * i + 1], 0};
    char* end = nullptr;
    out[i] = static_cast<uint8_t>(strtoul(buf, &end, 16));
    if (!end || *end) return false;
  }
  return true;
}

void randomHex(char* out33) {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  const String h = toHex(b, sizeof(b));
  memcpy(out33, h.c_str(), 32);
  out33[32] = '\0';
}

// Record format: pbkdf2_sha256$<iterations>$<salt hex>$<hash hex>
String makeRecord(const String& pass) {
  uint8_t salt[16], hash[32];
  esp_fill_random(salt, sizeof(salt));
  pbkdf2(reinterpret_cast<const uint8_t*>(pass.c_str()), pass.length(), salt, sizeof(salt), PBKDF2_ITERATIONS,
         hash, sizeof(hash));
  return String("pbkdf2_sha256$") + PBKDF2_ITERATIONS + "$" + toHex(salt, sizeof(salt)) + "$" +
         toHex(hash, sizeof(hash));
}

bool verifyRecord(const String& record, const String& pass) {
  const int a = record.indexOf('$');
  const int b = record.indexOf('$', a + 1);
  const int c = record.indexOf('$', b + 1);
  if (a < 0 || b < 0 || c < 0 || record.substring(0, a) != "pbkdf2_sha256") return false;
  const uint32_t iterations = record.substring(a + 1, b).toInt();
  uint8_t salt[16], expect[32], got[32];
  if (iterations == 0 || !fromHex(record.substring(b + 1, c), salt, sizeof(salt)) ||
      !fromHex(record.substring(c + 1), expect, sizeof(expect))) {
    return false;
  }
  pbkdf2(reinterpret_cast<const uint8_t*>(pass.c_str()), pass.length(), salt, sizeof(salt), iterations, got,
         sizeof(got));
  uint8_t diff = 0;  // constant-time compare
  for (size_t i = 0; i < sizeof(got); ++i) diff |= got[i] ^ expect[i];
  return diff == 0;
}

void applyClearAll() {
  if (!s_clearAll) return;
  s_clearAll = false;
  for (Session& s : s_sessions) {
    s.used = false;
    s.flashes.clear();
  }
}

}  // namespace

void storeRecord(const String& record) {
  settings::modify([&](settings::Data& d) { d.pwRecord = record; });
}

void begin() {
  if (settings::get().pwRecord.isEmpty()) {
    LOGI("auth", "no password stored, setting the default");
    storeRecord(makeRecord(DEFAULT_ADMIN_PASSWORD));
  }
}

bool checkPassword(const String& pass) { return verifyRecord(settings::get().pwRecord, pass); }

bool checkLogin(const String& user, const String& pass) {
  // Always hash, so the response time does not reveal a wrong username.
  const bool pwOk = checkPassword(pass);
  return pwOk && user == ADMIN_USERNAME;
}

bool setPassword(const String& pass) {
  storeRecord(makeRecord(pass));
  return true;
}

void resetPasswordToDefault() {
  storeRecord(makeRecord(DEFAULT_ADMIN_PASSWORD));
  endAllSessions();
}

Session* sessionFromCookie(const String& cookie) {
  applyClearAll();
  const int p = cookie.indexOf("vrata_session=");
  if (p < 0) return nullptr;
  const String token = cookie.substring(p + 14, p + 14 + 32);
  if (token.length() != 32) return nullptr;
  const uint32_t now = millis();
  for (Session& s : s_sessions) {
    if (!s.used || token != s.token) continue;
    if (now - s.lastSeenMs > SESSION_IDLE_TIMEOUT_S * 1000UL) {
      s.used = false;
      s.flashes.clear();
      return nullptr;
    }
    s.lastSeenMs = now;
    return &s;
  }
  return nullptr;
}

Session* createSession() {
  applyClearAll();
  Session* slot = nullptr;
  for (Session& s : s_sessions) {  // free slot, else the least recently used
    if (!s.used) {
      slot = &s;
      break;
    }
    if (!slot || s.lastSeenMs < slot->lastSeenMs) slot = &s;
  }
  slot->used = true;
  slot->flashes.clear();
  randomHex(slot->token);
  randomHex(slot->csrf);
  slot->lastSeenMs = millis();
  return slot;
}

void endSession(Session* s) {
  if (!s) return;
  s->used = false;
  s->flashes.clear();
}

void endAllSessions() { s_clearAll = true; }

bool lockedOut(uint32_t& secondsLeft) {
  const int32_t left = static_cast<int32_t>(s_lockUntilMs - millis());
  if (s_lockUntilMs != 0 && left > 0) {
    secondsLeft = (left + 999) / 1000;
    return true;
  }
  return false;
}

void noteFailure() {
  if (++s_failures >= LOGIN_MAX_FAILURES) {
    s_failures = 0;
    s_lockUntilMs = millis() + LOGIN_LOCKOUT_S * 1000UL;
    if (s_lockUntilMs == 0) s_lockUntilMs = 1;
    LOGW("auth", "too many failed logins, locked for %d s", LOGIN_LOCKOUT_S);
  }
}

void noteSuccess() {
  s_failures = 0;
  s_lockUntilMs = 0;
}

bool selfTest() {
  // Well-known PBKDF2-HMAC-SHA256 test vector: P="password", S="salt", c=2, dkLen=32
  static const uint8_t kExpected[32] = {0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3, 0x2d, 0x0a, 0xdf,
                                        0xf9, 0x28, 0xf0, 0x6d, 0xd0, 0x2a, 0x30, 0x3f, 0x8e, 0xf3, 0xc2,
                                        0x51, 0xdf, 0xd6, 0xe2, 0xd8, 0x5a, 0x95, 0x47, 0x4c, 0x43};
  uint8_t out[32];
  pbkdf2(reinterpret_cast<const uint8_t*>("password"), 8, reinterpret_cast<const uint8_t*>("salt"), 4, 2, out,
         sizeof(out));
  return memcmp(out, kExpected, sizeof(out)) == 0;
}

}  // namespace auth
