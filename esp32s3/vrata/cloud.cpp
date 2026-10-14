#include "cloud.h"

#include <WiFi.h>
#include <cJSON.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_random.h>

#include <deque>

#include "applog.h"
#include "config.h"
#include "controller.h"
#include "fetch.h"
#include "netmgr.h"
#include "portal.h"
#include "settings.h"
#include "wdt.h"

// The Arduino core would mark a freshly updated image valid at boot; we
// confirm it ourselves once it has proven it works (see otaVerifyTick()).
extern "C" bool verifyRollbackLater() { return true; }

namespace cloud {

namespace {

// The server reads the version from this marker inside the uploaded .bin.
__attribute__((used)) const char kFwMarker[] = "VRATA_FW_VERSION=" FW_VERSION ";";

struct LogEntry {
  uint32_t t;
  char type[16];
  char msg[112];
};

SemaphoreHandle_t s_mutex = nullptr;
std::deque<LogEntry> s_log;
String s_mac;
String s_bootId;
volatile bool s_syncRequested = true;
uint32_t s_nextSyncMs = 0;
uint32_t s_lastOkMs = 0;  // millis() of the last good reply (grace period at boot)
int64_t s_lastOkAt = 0;
bool s_okThisBoot = false;
String s_state = "starting";
String s_lastError;
bool s_pendingVerify = false;
uint8_t s_otaSrc = 0;
String s_otaError;
String s_otaFailVersion;
uint8_t s_otaFails = 0;

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

void setState(const String& state, const String& error) {
  Lock l;
  s_state = state;
  s_lastError = error;
}

const char* jstr(cJSON* o, const char* key, const char* def = "") {
  cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

long jint(cJSON* o, const char* key, long def) {
  cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? static_cast<long>(v->valuedouble) : def;
}

bool jbool(cJSON* o, const char* key, bool def) {
  cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

long clampL(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool validName(const String& n) {
  if (n.isEmpty() || n.length() > 32) return false;
  for (size_t i = 0; i < n.length(); ++i) {
    const char c = n[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

// ------------------------------------------------------------- report -----

cJSON* buildReport(const settings::Data& cfg) {
  const ctl::Status st = ctl::status();
  const netmgr::Info net = netmgr::info();
  const portal::Info pi = portal::info();
  cJSON* r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "mode", static_cast<int>(st.mode));
  cJSON_AddStringToObject(r, "mode_label", ctl::modeLabel(st.mode));
  cJSON_AddBoolToObject(r, "door_open", st.relayOn);
  cJSON_AddStringToObject(r, "open_reason", st.openReason.c_str());
  cJSON_AddBoolToObject(r, "time_valid", st.timeValid);
  cJSON_AddNumberToObject(r, "now", static_cast<double>(st.nowUtc));
  if (st.eventActive) {
    cJSON* e = cJSON_AddObjectToObject(r, "event");
    cJSON_AddStringToObject(e, "summary", st.eventSummary.c_str());
    cJSON_AddNumberToObject(e, "end", static_cast<double>(st.eventEnd));
  }
  if (st.hasNext) {
    cJSON* e = cJSON_AddObjectToObject(r, "next");
    cJSON_AddStringToObject(e, "summary", st.nextSummary.c_str());
    cJSON_AddNumberToObject(e, "start", static_cast<double>(st.nextStart));
  }
  cJSON* cal = cJSON_AddObjectToObject(r, "cal");
  cJSON_AddStringToObject(cal, "source", schedule::sourceName(st.sched.source));
  cJSON_AddBoolToObject(cal, "ok", st.sched.ok);
  cJSON_AddNumberToObject(cal, "events", st.sched.events);
  cJSON_AddNumberToObject(cal, "occ", st.sched.occurrences);
  cJSON_AddNumberToObject(cal, "bookings", st.sched.bookings);
  cJSON_AddNumberToObject(cal, "skipped", st.sched.skipped);
  cJSON_AddStringToObject(cal, "err", st.sched.ok ? "" : st.sched.error.c_str());
  cJSON_AddStringToObject(cal, "last_dl", st.lastDownload.c_str());
  cJSON_AddNumberToObject(cal, "last_dl_at", static_cast<double>(st.lastDownloadAt));
  cJSON* w = cJSON_AddObjectToObject(r, "wifi");
  cJSON_AddStringToObject(w, "ssid", net.ssid.c_str());
  cJSON_AddNumberToObject(w, "rssi", net.rssi);
  cJSON_AddBoolToObject(w, "ap", net.apActive && net.apMode == "setup");
  cJSON_AddNumberToObject(w, "rejected_rev", cfg.wifiRejectedRev);
  cJSON* p = cJSON_AddObjectToObject(r, "portal");
  cJSON_AddBoolToObject(p, "enabled", pi.enabled);
  cJSON_AddStringToObject(p, "ssid", pi.ssid.c_str());
  cJSON_AddBoolToObject(p, "ap", pi.apUp);
  cJSON_AddNumberToObject(p, "phones", pi.phones);
  cJSON_AddNumberToObject(p, "unlock_left", pi.unlockLeftS);
  cJSON_AddNumberToObject(p, "paused_left", pi.pausedLeftS);
  cJSON_AddNumberToObject(p, "users", pi.users);
  cJSON_AddNumberToObject(p, "bookings", st.sched.bookings);
  cJSON_AddNumberToObject(r, "heap", ESP.getFreeHeap());
  cJSON_AddNumberToObject(r, "min_heap", ESP.getMinFreeHeap());
  cJSON_AddNumberToObject(r, "uptime_s", millis() / 1000UL);
  cJSON_AddNumberToObject(r, "config_rev", cfg.configRev);
  cJSON_AddNumberToObject(r, "wifi_rev", cfg.wifiRev);
  cJSON* o = cJSON_AddObjectToObject(r, "ota");
  cJSON_AddBoolToObject(o, "pending_verify", s_pendingVerify);
  cJSON_AddStringToObject(o, "bad", cfg.otaBad.c_str());
  cJSON_AddStringToObject(o, "error", s_otaError.c_str());
  return r;
}

// ------------------------------------------------------------ firmware -----

void runOta(const String& url, const String& version, const String& md5, size_t size) {
  const settings::Data cfg = settings::get();
  LOGW("ota", "server assigned firmware %s (running %s), installing", version.c_str(), FW_VERSION);
  const fetch::Result r = fetch::downloadFirmware(url, s_mac, cfg.token, md5, size);
  if (!r.ok) {
    s_otaError = version + ": " + r.error;
    s_otaFails = (s_otaFailVersion == version) ? s_otaFails + 1 : 1;
    s_otaFailVersion = version;
    LOGE("ota", "update to %s failed (%u/3): %s", version.c_str(), s_otaFails, r.error.c_str());
    logEvent("ota", "Posodobitev na " + version + " ni uspela: " + r.error);
    if (s_otaFails >= 3) settings::modify([&](settings::Data& d) { d.otaBad = version; });
    return;
  }
  settings::modify([&](settings::Data& d) {
    d.otaTry = version;
    d.otaSrc = 1;
  });
  LOGW("ota", "firmware %s installed, restarting", version.c_str());
  delay(500);
  ESP.restart();
}

void confirmImage() {
  esp_ota_mark_app_valid_cancel_rollback();
  s_pendingVerify = false;
  settings::modify([](settings::Data& d) {
    d.otaTry = "";
    d.otaSrc = 0;
  });
  LOGI("ota", "firmware %s confirmed", FW_VERSION);
  logEvent("ota", String("Programska oprema ") + FW_VERSION + " deluje in je potrjena.");
}

void otaVerifyTick() {
  if (!s_pendingVerify) return;
  if (s_otaSrc == 1) {  // from the server: it must be able to reach the server
    if (s_okThisBoot) {
      confirmImage();
    } else if (millis() > OTA_CONFIRM_TIMEOUT_MS) {
      LOGE("ota", "new firmware could not reach the server in time - rolling back");
      delay(200);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  } else if (millis() > OTA_LOCAL_CONFIRM_MS) {
    confirmImage();  // local upload: it ran long enough without crashing
  }
}

// ------------------------------------------------------- configuration -----

void applyConfig(cJSON* c) {
  const settings::Data before = settings::get();
  bool calChanged = false, netChanged = false, portalChanged = false;
  const long status = jint(c, "status", -1);
  const String name = jstr(c, "name");
  const String remote = jstr(c, "remote_ical");
  const String local = jstr(c, "local_ical");
  const long poll = clampL(jint(c, "poll_s", DEFAULT_POLL_S), 30, 3600);
  const long cal = clampL(jint(c, "calendar_s", DEFAULT_CALENDAR_S), 60, 3600);
  const uint8_t setupAp = String(jstr(c, "setup_ap", "auto")) == "button" ? 1 : 0;
  const long rev = jint(c, "rev", 0);

  cJSON* p = cJSON_GetObjectItemCaseSensitive(c, "portal");
  const bool pEnabled = cJSON_IsObject(p) && jbool(p, "enabled", false);
  String pSsid = cJSON_IsObject(p) ? jstr(p, "ssid", PORTAL_DEFAULT_SSID) : PORTAL_DEFAULT_SSID;
  if (pSsid.isEmpty() || pSsid.length() > 32) pSsid = PORTAL_DEFAULT_SSID;
  const long pUnlock = cJSON_IsObject(p) ? clampL(jint(p, "unlock_s", 600), 10, 3600) : 600;
  // Older servers do not send pause_s: default to "back one minute before the door locks".
  const long pPause = cJSON_IsObject(p) ? clampL(jint(p, "pause_s", pUnlock - 60), 0, pUnlock) : 0;
  const long pBefore = cJSON_IsObject(p) ? clampL(jint(p, "before_min", 15), 0, 240) : 15;
  const long pAfter = cJSON_IsObject(p) ? clampL(jint(p, "after_min", 15), 0, 240) : 15;
  String pUsers;
  cJSON* users = cJSON_IsObject(p) ? cJSON_GetObjectItemCaseSensitive(p, "users") : nullptr;
  int count = 0;
  cJSON* u;
  cJSON_ArrayForEach(u, users) {
    if (!cJSON_IsString(u) || ++count > PORTAL_MAX_USERS) continue;
    pUsers += u->valuestring;
    pUsers += '\n';
  }

  settings::modify([&](settings::Data& d) {
    if (validName(name) && name != d.deviceName) {
      d.deviceName = name;
      netChanged = true;
    }
    if (status >= 0 && status <= 3) d.serverStatus = static_cast<int8_t>(status);
    if (remote != d.remoteCalUrl) {
      d.remoteCalUrl = remote;
      calChanged = true;
    }
    if (local != d.localUrl) {
      d.localUrl = local;
      calChanged = true;
    }
    d.pollS = static_cast<uint16_t>(poll);
    d.calS = static_cast<uint16_t>(cal);
    if (setupAp != d.setupAp) {
      d.setupAp = setupAp;
      netChanged = true;
    }
    if (pEnabled != d.portalEnabled || pSsid != d.portalSsid) netChanged = true;
    portalChanged = netChanged || pEnabled != d.portalEnabled || pSsid != d.portalSsid ||
                    pUnlock != d.portalUnlockS || pPause != d.portalPauseS || pBefore != d.pinBeforeMin ||
                    pAfter != d.pinAfterMin || pUsers != d.portalUsers;
    d.portalEnabled = pEnabled;
    d.portalSsid = pSsid;
    d.portalUnlockS = static_cast<uint16_t>(pUnlock);
    d.portalPauseS = static_cast<uint16_t>(pPause);
    d.pinBeforeMin = static_cast<uint16_t>(pBefore);
    d.pinAfterMin = static_cast<uint16_t>(pAfter);
    d.portalUsers = pUsers;
    if (rev > 0) d.configRev = static_cast<uint32_t>(rev);
  });

  if (status >= 0 && status <= 3) ctl::applyServerStatus(static_cast<int>(status));
  ctl::setOpenUntil(static_cast<int64_t>(jint(c, "open_until", 0)));
  if (calChanged) ctl::requestCalendarRefresh();
  if (portalChanged) portal::reconfigure();
  if (netChanged) netmgr::requestReconfigure();
  if (before.configRev != static_cast<uint32_t>(rev) && rev > 0) LOGI("cloud", "settings revision %ld applied", rev);

  // Wi-Fi networks from the server (only when the admin manages them there).
  const long wifiRev = jint(c, "wifi_rev", 0);
  cJSON* wifi = cJSON_GetObjectItemCaseSensitive(c, "wifi");
  if (wifiRev > 0 && cJSON_IsArray(wifi)) {
    const settings::Data now = settings::get();
    if (static_cast<uint32_t>(wifiRev) != now.wifiRev && static_cast<uint32_t>(wifiRev) != now.wifiRejectedRev) {
      settings::WifiNet nets[WIFI_MAX_NETWORKS];
      int n = 0;
      cJSON* w;
      cJSON_ArrayForEach(w, wifi) {
        if (n >= WIFI_MAX_NETWORKS) break;
        const String ssid = jstr(w, "ssid");
        if (ssid.isEmpty()) continue;
        nets[n].ssid = ssid;
        nets[n].pass = jstr(w, "pass");
        ++n;
      }
      if (n > 0) netmgr::applyServerWifi(nets, n, static_cast<uint32_t>(wifiRev));
    }
  }

  // Firmware assigned on the server.
  cJSON* fw = cJSON_GetObjectItemCaseSensitive(c, "firmware");
  if (cJSON_IsObject(fw)) {
    const String version = jstr(fw, "version");
    if (!version.isEmpty() && version != FW_VERSION && version != settings::get().otaBad) {
      runOta(jstr(fw, "url"), version, jstr(fw, "md5"), static_cast<size_t>(jint(fw, "size", 0)));
    }
  }
}

// One exchange with the server. Returns true if the server answered.
bool syncOnce() {
  const settings::Data cfg = settings::get();
  if (cfg.serverUrl.isEmpty()) {
    setState("no server URL set", "");
    return false;
  }
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "mac", s_mac.c_str());
  if (!cfg.token.isEmpty()) cJSON_AddStringToObject(root, "token", cfg.token.c_str());
  cJSON_AddStringToObject(root, "pair_code", cfg.pairCode.c_str());
  cJSON_AddStringToObject(root, "fw", FW_VERSION);
  cJSON_AddStringToObject(root, "boot_id", s_bootId.c_str());
  cJSON_AddStringToObject(root, "reset_reason", resetReasonText());
  cJSON_AddStringToObject(root, "local_ip", WiFi.localIP().toString().c_str());
  if (!cfg.token.isEmpty()) cJSON_AddItemToObject(root, "report", buildReport(cfg));
  size_t sent = 0;
  cJSON* log = cJSON_AddArrayToObject(root, "log");
  {
    Lock l;
    for (const LogEntry& e : s_log) {
      cJSON* item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "t", e.t);
      cJSON_AddStringToObject(item, "type", e.type);
      cJSON_AddStringToObject(item, "msg", e.msg);
      cJSON_AddItemToArray(log, item);
      if (++sent >= 40) break;
    }
  }
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) {
    setState("out of memory", "JSON");
    return false;
  }
  const String body(txt);
  cJSON_free(txt);

  String resp;
  const fetch::Result r = fetch::postJson(cfg.serverUrl + "/api/door.php", body, resp, MAX_API_BYTES);
  if (!r.ok) {
    if (r.httpCode == 401 && !cfg.token.isEmpty()) {
      settings::modify([](settings::Data& d) { d.token = ""; });
      LOGW("cloud", "server no longer accepts this door's key - waiting to be adopted again");
      setState("key rejected, waiting for adoption", r.error);
      s_syncRequested = true;
      return true;
    }
    setState("server unreachable", r.error);
    return false;
  }

  cJSON* j = cJSON_Parse(resp.c_str());
  if (!j) {
    setState("invalid reply from server", "not JSON");
    return false;
  }
  const String state = jstr(j, "state");
  bool answered = true;
  if (state == "pending") {
    setState(String("waiting for adoption (code ") + cfg.pairCode + ")", "");
  } else if (state == "adopted") {
    const String token = jstr(j, "token");
    if (token.length() == 64) {
      settings::modify([&](settings::Data& d) { d.token = token; });
      LOGI("cloud", "adopted by the server");
      logEvent("adopt", "Naprava je prevzela ključ.");
      setState("adopted", "");
      s_syncRequested = true;
    }
  } else if (state == "active") {
    cJSON* c = cJSON_GetObjectItemCaseSensitive(j, "config");
    if (cJSON_IsObject(c)) {
      s_okThisBoot = true;
      s_lastOkMs = millis();
      s_lastOkAt = static_cast<int64_t>(time(nullptr));
      setState("connected", "");
      applyConfig(c);  // may restart the board for a firmware update
    }
  } else {
    answered = false;
    setState("unexpected reply from server", jstr(j, "error"));
  }
  cJSON_Delete(j);
  if (answered) {  // the server has (or does not need) these log entries
    Lock l;
    for (size_t i = 0; i < sent && !s_log.empty(); ++i) s_log.pop_front();
  }
  return answered;
}

}  // namespace

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "vklop napajanja";
    case ESP_RST_EXT: return "tipka RESET";
    case ESP_RST_SW: return "programski ponovni zagon";
    case ESP_RST_PANIC: return "napaka programa";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "padec napetosti";
    case ESP_RST_USB: return "USB";
    default: return "drugo";
  }
}

void bootCheck() {
  s_mutex = xSemaphoreCreateMutex();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  s_mac = buf;
  snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(esp_random()));
  s_bootId = buf;
  s_lastOkMs = millis();  // grace period: "managed" until the first polls fail

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  s_pendingVerify = running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  const settings::Data d = settings::get();
  s_otaSrc = d.otaSrc;
  if (!d.otaTry.isEmpty() && d.otaTry != FW_VERSION) {
    // We were updating to otaTry but run something else: it was rolled back.
    LOGE("ota", "firmware %s did not work, back on %s", d.otaTry.c_str(), FW_VERSION);
    logEvent("ota", "Različica " + d.otaTry + " ni delovala, naprava se je vrnila na " + FW_VERSION + ".");
    const String bad = d.otaTry;
    settings::modify([&](settings::Data& x) {
      x.otaBad = bad;
      x.otaTry = "";
      x.otaSrc = 0;
    });
    s_otaSrc = 0;
  } else if (!s_pendingVerify && d.otaSrc != 0) {
    settings::modify([](settings::Data& x) {
      x.otaTry = "";
      x.otaSrc = 0;
    });
  }
  if (s_pendingVerify) {
    LOGW("ota", "running new firmware %s: %s", FW_VERSION,
         s_otaSrc == 1 ? "must reach the server within 10 min" : "confirmed after 3 min without problems");
  }
  LOGI("main", "%s, door MAC %s, pairing code %s", kFwMarker, s_mac.c_str(), d.pairCode.c_str());
}

void tick() {
  otaVerifyTick();
  if (!netmgr::online()) return;
  if (!s_syncRequested && static_cast<int32_t>(millis() - s_nextSyncMs) < 0) return;
  s_syncRequested = false;
  const bool ok = syncOnce();
  const settings::Data cfg = settings::get();
  uint32_t interval = cfg.token.isEmpty() ? 30000UL : cfg.pollS * 1000UL;  // pending doors ask every 30 s
  if (!ok && interval > NET_RETRY_AFTER_FAILURE_MS) interval = NET_RETRY_AFTER_FAILURE_MS;
  s_nextSyncMs = millis() + interval;
}

void requestSync() { s_syncRequested = true; }

bool adopted() { return !settings::get().token.isEmpty(); }

bool managed() {
  const settings::Data cfg = settings::get();
  if (cfg.token.isEmpty()) return false;
  uint32_t window = OFFLINE_AFTER_FAILURES * static_cast<uint32_t>(cfg.pollS) * 1000UL;
  if (window < 180000UL) window = 180000UL;
  return millis() - s_lastOkMs < window;
}

void logEvent(const char* type, const String& msg) {
  if (!s_mutex) return;
  LogEntry e;
  e.t = static_cast<uint32_t>(time(nullptr));
  strlcpy(e.type, type, sizeof(e.type));
  strlcpy(e.msg, msg.c_str(), sizeof(e.msg));
  Lock l;
  if (s_log.size() >= 40) s_log.pop_front();
  s_log.push_back(e);
}

void noteLocalOta() {
  settings::modify([](settings::Data& d) {
    d.otaTry = "";
    d.otaSrc = 2;
  });
}

Info info() {
  Info i;
  const settings::Data cfg = settings::get();
  i.serverUrl = cfg.serverUrl;
  i.adopted = !cfg.token.isEmpty();
  i.managed = managed();
  i.pairCode = cfg.pairCode;
  i.otaPendingVerify = s_pendingVerify;
  Lock l;
  i.state = s_state;
  i.lastError = s_lastError;
  i.lastOkAt = s_lastOkAt;
  i.otaError = s_otaError;
  return i;
}

}  // namespace cloud
