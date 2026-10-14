#include "fetch.h"

#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <Update.h>

#include <memory>
#include <utility>
#include <vector>

#include "applog.h"
#include "config.h"
#include "storage.h"
#include "wdt.h"

namespace fetch {

namespace {

SemaphoreHandle_t netMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();  // thread-safe static init
  return m;
}

struct NetLock {
  bool ok;
  // Must stay below WDT_TIMEOUT_S: waiting does not feed the watchdog.
  NetLock() { ok = xSemaphoreTake(netMutex(), pdMS_TO_TICKS(45000)) == pdTRUE; }
  ~NetLock() {
    if (ok) xSemaphoreGive(netMutex());
  }
};

// Write-only Stream that enforces a size limit and feeds the watchdog while
// HTTPClient::writeToStream() (which also decodes chunked encoding) runs.
class LimitedSink : public Stream {
 public:
  explicit LimitedSink(size_t limit) : limit_(limit) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override {
    if (total_ + n > limit_) {
      overflow_ = true;
      return 0;
    }
    const size_t w = sink(buf, n);
    total_ += w;
    wdt::feed();
    return w;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t total() const { return total_; }
  bool overflow() const { return overflow_; }

 protected:
  virtual size_t sink(const uint8_t* buf, size_t n) = 0;

 private:
  size_t limit_;
  size_t total_ = 0;
  bool overflow_ = false;
};

class FileSink : public LimitedSink {
 public:
  FileSink(fs::File& f, size_t limit) : LimitedSink(limit), f_(f) {}

 protected:
  size_t sink(const uint8_t* buf, size_t n) override { return f_.write(buf, n); }

 private:
  fs::File& f_;
};

class StringSink : public LimitedSink {
 public:
  StringSink(String& s, size_t limit) : LimitedSink(limit), s_(s) {}

 protected:
  size_t sink(const uint8_t* buf, size_t n) override {
    return s_.concat(reinterpret_cast<const char*>(buf), n) ? n : 0;
  }

 private:
  String& s_;
};

class UpdateSink : public LimitedSink {
 public:
  explicit UpdateSink(size_t limit) : LimitedSink(limit) {}

 protected:
  size_t sink(const uint8_t* buf, size_t n) override { return Update.write(const_cast<uint8_t*>(buf), n); }
};

struct Req {
  const char* method = "GET";
  const String* body = nullptr;
  std::vector<std::pair<String, String>> headers;
};

String resolveLocation(const String& base, const String& loc) {
  if (loc.startsWith("http://") || loc.startsWith("https://")) return loc;
  const int schemeEnd = base.indexOf("://");
  if (loc.startsWith("//")) return base.substring(0, schemeEnd + 1) + loc;
  const int hostEnd = base.indexOf('/', schemeEnd + 3);
  const String origin = hostEnd < 0 ? base : base.substring(0, hostEnd);
  if (loc.startsWith("/")) return origin + loc;
  const int lastSlash = base.lastIndexOf('/');
  return (lastSlash > schemeEnd + 2 ? base.substring(0, lastSlash + 1) : origin + "/") + loc;
}

// Printable host part of a URL for log lines (never logs private paths:
// Google's secret calendar URLs contain the access key).
String hostOf(const String& url) {
  const int s = url.indexOf("://");
  if (s < 0) return url;
  const int e = url.indexOf('/', s + 3);
  return e < 0 ? url.substring(s + 3) : url.substring(s + 3, e);
}

// One request with manual redirect handling (a fresh client per hop, so an
// http -> https redirect works, which HTTPClient's own follower refuses).
Result request(const String& urlIn, LimitedSink& sink, const Req& req) {
  Result r;
  String url = urlIn;
  url.trim();
  if (url.isEmpty()) {
    r.error = "no URL set";
    return r;
  }
  for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; ++hop) {
    const bool https = url.startsWith("https://");
    std::unique_ptr<NetworkClient> client;
    NetworkClientSecure* secure = nullptr;
    if (https) {
      secure = new NetworkClientSecure();
#if TLS_VERIFY
      secure->useBuiltinCACertBundle();
#else
      secure->setInsecure();
#endif
      secure->setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
      client.reset(secure);
    } else if (url.startsWith("http://")) {
      client.reset(new NetworkClient());
    } else {
      r.error = "URL must start with http:// or https://";
      return r;
    }

    HTTPClient http;  // destroyed before `client` (declared later)
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(FW_NAME "/" FW_VERSION);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, 1);
    if (!http.begin(*client, url)) {
      r.error = "invalid URL";
      return r;
    }
    for (const auto& h : req.headers) http.addHeader(h.first, h.second);
    wdt::feed();
    int code;
    if (req.body) {
      http.addHeader("Content-Type", "application/json");
      code = http.sendRequest(req.method, *req.body);
    } else {
      code = http.GET();
    }
    wdt::feed();
    r.httpCode = code;

    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      const String loc = http.header("Location");
      http.end();
      if (loc.isEmpty()) {
        r.error = "redirect without Location header";
        return r;
      }
      url = resolveLocation(url, loc);
      continue;
    }
    if (code != HTTP_CODE_OK) {
      if (code < 0) {
        r.error = HTTPClient::errorToString(code);
        char tlsErr[96];
        if (secure && secure->lastError(tlsErr, sizeof(tlsErr)) != 0) r.error += String(" (TLS: ") + tlsErr + ")";
      } else {
        r.error = String("HTTP ") + code;
      }
      http.end();
      return r;
    }
    const int n = http.writeToStream(&sink);
    http.end();
    if (sink.overflow()) {
      r.error = "response too large";
      return r;
    }
    if (n < 0) {
      r.error = HTTPClient::errorToString(n);
      return r;
    }
    r.ok = true;
    r.bytes = sink.total();
    return r;
  }
  r.error = "too many redirects";
  return r;
}

// Same check as app.py (first line must be BEGIN:VCALENDAR) plus a check
// for END:VCALENDAR at the end, so a cut-off download is never used.
bool looksLikeCalendar(const char* path, String& why) {
  fs::File f = LittleFS.open(path, "r");
  if (!f) {
    why = "cannot read downloaded file";
    return false;
  }
  char head[40] = {0};
  f.readBytes(head, sizeof(head) - 1);
  String first(head);
  if (first.startsWith("\xEF\xBB\xBF")) first.remove(0, 3);  // UTF-8 BOM
  const int nl = first.indexOf('\n');
  if (nl >= 0) first.remove(nl);
  first.trim();
  if (!first.equalsIgnoreCase("BEGIN:VCALENDAR")) {
    f.close();
    why = "The file is not a valid ICS calendar file.";
    return false;
  }
  const size_t size = f.size();
  char tail[48] = {0};
  f.seek(size > sizeof(tail) - 1 ? size - (sizeof(tail) - 1) : 0);
  f.readBytes(tail, sizeof(tail) - 1);
  f.close();
  String end(tail);
  end.toUpperCase();
  if (end.indexOf("END:VCALENDAR") < 0) {
    why = "The calendar download is incomplete (no END:VCALENDAR).";
    return false;
  }
  return true;
}

}  // namespace

Result postJson(const String& url, const String& body, String& response, size_t maxBytes) {
  NetLock lock;
  Result r;
  if (!lock.ok) {
    r.error = "network busy";
    return r;
  }
  response = String();
  StringSink sink(response, maxBytes);
  Req req;
  req.method = "POST";
  req.body = &body;
  r = request(url, sink, req);
  if (!r.ok) LOGW("http", "POST %s failed: %s", hostOf(url).c_str(), r.error.c_str());
  return r;
}

Result downloadCalendar(const String& url, const char* path) {
  NetLock lock;
  Result r;
  if (!lock.ok) {
    r.error = "network busy";
    return r;
  }
  if (!storage::mounted()) {
    r.error = "no filesystem";
    return r;
  }
  const String tmp = String(path) + ".tmp";
  {
    fs::File f = LittleFS.open(tmp, "w");
    if (!f) {
      r.error = "cannot create temporary file";
      return r;
    }
    const size_t free = storage::freeBytes();
    const size_t room = free > 16384 ? free - 16384 : 0;
    FileSink sink(f, room < MAX_ICS_BYTES ? room : MAX_ICS_BYTES);
    r = request(url, sink, Req());
    f.close();
  }
  String why;
  if (r.ok && !looksLikeCalendar(tmp.c_str(), why)) {
    r.ok = false;
    r.error = why;
  }
  if (!r.ok) {
    LittleFS.remove(tmp);
    LOGW("http", "calendar from %s rejected: %s", hostOf(url).c_str(), r.error.c_str());
    return r;
  }
  {
    storage::Lock fsLock;  // not while the schedule is being parsed
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) {
      r.ok = false;
      r.error = "could not store the calendar";
    }
  }
  if (r.ok) LOGI("http", "calendar from %s saved to %s (%u bytes)", hostOf(url).c_str(), path, static_cast<unsigned>(r.bytes));
  return r;
}

Result downloadFirmware(const String& url, const String& mac, const String& token, const String& md5, size_t size) {
  NetLock lock;
  Result r;
  if (!lock.ok) {
    r.error = "network busy";
    return r;
  }
  if (!Update.begin(size ? size : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    r.error = String("no room for the update: ") + Update.errorString();
    return r;
  }
  if (md5.length() == 32) Update.setMD5(md5.c_str());
  UpdateSink sink(size ? size : 4UL * 1024UL * 1024UL);
  Req req;
  req.headers.push_back(std::make_pair(String("X-Door-Mac"), mac));
  req.headers.push_back(std::make_pair(String("X-Door-Token"), token));
  LOGI("ota", "downloading firmware from %s (%u bytes)", hostOf(url).c_str(), static_cast<unsigned>(size));
  r = request(url, sink, req);
  if (!r.ok) {
    Update.abort();
    return r;
  }
  if (!Update.end(true)) {  // also checks the MD5
    r.ok = false;
    r.error = String("update rejected: ") + Update.errorString();
  }
  return r;
}

}  // namespace fetch
