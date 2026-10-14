#include "schedule.h"

#include <LittleFS.h>

#include <vector>

#include "applog.h"
#include "config.h"
#include "storage.h"
#include "tzconv.h"
#include "wdt.h"

namespace schedule {

namespace {

SemaphoreHandle_t s_mutex = nullptr;
std::vector<ical::Occurrence> s_occ;
Info s_info;

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

class FileSource : public ical::ByteSource {
 public:
  explicit FileSource(fs::File& f) : f_(f) {}
  int read(uint8_t* buf, size_t len) override { return static_cast<int>(f_.read(buf, len)); }
  bool rewind() override { return f_.seek(0); }

 private:
  fs::File& f_;
};

void onWarn(const char* msg) { LOGW("ical", "%s", msg); }

void onYield() {
  wdt::feed();
  vTaskDelay(1);  // let the web task breathe while a big file is parsed
}

void publish(Source src, std::vector<ical::Occurrence>& occ, const Info& inf) {
  Lock l;
  s_occ.swap(occ);  // old list is freed by the caller, outside the lock
  s_info = inf;
  s_info.source = src;
}

}  // namespace

void begin() { s_mutex = xSemaphoreCreateMutex(); }

const char* pathFor(Source src) {
  switch (src) {
    case Source::Local: return storage::kLocalIcs;
    case Source::Remote: return storage::kRemoteIcs;
    default: return nullptr;
  }
}

const char* sourceName(Source src) {
  switch (src) {
    case Source::Local: return "local";
    case Source::Remote: return "remote";
    default: return "none";
  }
}

void clear() {
  std::vector<ical::Occurrence> empty;
  publish(Source::None, empty, Info());
}

bool rebuild(Source src, int64_t nowUtc, int64_t keepEndedS) {
  Info inf;
  inf.builtAt = nowUtc;
  std::vector<ical::Occurrence> occ;
  const char* path = pathFor(src);
  if (!path) {
    publish(src, occ, inf);
    return true;
  }

  const uint32_t t0 = millis();
  {
    storage::Lock fsLock;
    fs::File f = storage::mounted() ? LittleFS.open(path, "r") : fs::File();
    if (!f) {
      inf.error = "no calendar downloaded yet";
    } else {
      inf.fileSize = f.size();
      FileSource source(f);
      ical::Options opt;
      opt.windowStart = nowUtc - WINDOW_PAST_S;
      opt.windowEnd = nowUtc + WINDOW_FUTURE_S;
      opt.dropEndedBefore = nowUtc - keepEndedS;
      opt.deviceTz = &tz::deviceRule();
      opt.allDayActivates = ALLDAY_EVENTS_ACTIVATE != 0;
      opt.defaultDurationS = DEFAULT_EVENT_DURATION_S;
      opt.maxOccurrences = MAX_OCCURRENCES;
      opt.yield = onYield;
      opt.warn = onWarn;
      ical::Stats st;
      if (ical::expand(source, opt, occ, st)) {
        inf.ok = true;
        inf.events = st.events;
        inf.occurrences = st.occurrences;
        inf.skipped = st.skippedAllDay + st.skippedTz + st.unsupportedRules;
        inf.truncated = st.truncated;
        for (const ical::Occurrence& o : occ) inf.bookings += o.user[0] ? 1 : 0;
        LOGI("sched", "%s calendar: %u events, %u upcoming occurrences (%u bookings, %u skipped%s) in %lu ms",
             sourceName(src), static_cast<unsigned>(st.events), static_cast<unsigned>(st.occurrences),
             static_cast<unsigned>(inf.bookings), static_cast<unsigned>(inf.skipped),
             st.truncated ? ", list truncated" : "", static_cast<unsigned long>(millis() - t0));
      } else {
        inf.error = "stored file is not an iCalendar file";
      }
      f.close();
    }
  }
  if (!inf.ok) LOGW("sched", "%s calendar: %s", sourceName(src), inf.error.c_str());
  publish(src, occ, inf);
  return inf.ok;
}

bool activeAt(Source src, int64_t nowUtc, bool bookingsAreAccessOnly, ical::Occurrence* which) {
  Lock l;
  if (s_info.source != src || src == Source::None) return false;  // list belongs to another mode
  for (const ical::Occurrence& o : s_occ) {
    if (o.start > nowUtc) break;  // sorted by start
    if (bookingsAreAccessOnly && o.user[0]) continue;
    if (nowUtc <= o.end) {        // same inclusive test as before
      if (which) *which = o;
      return true;
    }
  }
  return false;
}

bool findBooking(Source src, const String& user, const String& pin, int64_t nowUtc, int64_t beforeS, int64_t afterS,
                 ical::Occurrence* out) {
  Lock l;
  if (s_info.source != src || src == Source::None) return false;
  const ical::Occurrence* o = ical::findBooking(s_occ, user.c_str(), pin.c_str(), nowUtc, beforeS, afterS);
  if (o && out) *out = *o;
  return o != nullptr;
}

bool nextAfter(Source src, int64_t nowUtc, ical::Occurrence& out) {
  Lock l;
  if (s_info.source != src || src == Source::None) return false;
  for (const ical::Occurrence& o : s_occ) {
    if (o.start > nowUtc) {
      out = o;
      return true;
    }
  }
  return false;
}

Info info() {
  Lock l;
  return s_info;
}

}  // namespace schedule
