#include "applog.h"

#include <stdarg.h>
#include <sys/time.h>

#include "civil.h"
#include "config.h"
#include "tzconv.h"

namespace applog {

namespace {

SemaphoreHandle_t s_mutex = nullptr;
char s_ring[LOG_RING_BYTES];
size_t s_head = 0;     // next write position
bool s_wrapped = false;

void ringAppend(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    s_ring[s_head++] = s[i];
    if (s_head >= sizeof(s_ring)) {
      s_head = 0;
      s_wrapped = true;
    }
  }
}

}  // namespace

void begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

void write(char level, const char* tag, const char* fmt, ...) {
  char msg[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char stamp[24];
  const time_t now = time(nullptr);
  if (now > 1704067200) {  // clock set by NTP -> local wall time
    const civil::DateTime t = civil::fromSeconds(tz::utcToLocal(tz::deviceRule(), now));
    snprintf(stamp, sizeof(stamp), "%02u.%02u. %02u:%02u:%02u", t.d, t.mo, t.h, t.mi, t.s);
  } else {
    snprintf(stamp, sizeof(stamp), "+%lu.%03lus", millis() / 1000UL, millis() % 1000UL);
  }

  char line[280];
  const int n = snprintf(line, sizeof(line), "%s %c [%s] %s\n", stamp, level, tag, msg);
  const size_t len = n < 0 ? 0 : (static_cast<size_t>(n) >= sizeof(line) ? sizeof(line) - 1 : static_cast<size_t>(n));

  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
  Serial.write(reinterpret_cast<const uint8_t*>(line), len);
  ringAppend(line, len);
  if (s_mutex) xSemaphoreGive(s_mutex);
}

String dump() {
  String out;
  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
  out.reserve(sizeof(s_ring) + 1);
  if (s_wrapped) {
    // skip the partial first line
    size_t start = s_head;
    while (start < sizeof(s_ring) && s_ring[start] != '\n') ++start;
    if (start < sizeof(s_ring)) out.concat(s_ring + start + 1, sizeof(s_ring) - start - 1);
  }
  out.concat(s_ring, s_head);
  if (s_mutex) xSemaphoreGive(s_mutex);
  return out;
}

}  // namespace applog
