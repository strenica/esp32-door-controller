#include "controller.h"

#include "applog.h"
#include "civil.h"
#include "cloud.h"
#include "config.h"
#include "fetch.h"
#include "netmgr.h"
#include "portal.h"
#include "relay.h"
#include "settings.h"
#include "storage.h"
#include "tzconv.h"
#include "wdt.h"

namespace ctl {

namespace {

const EventBits_t EV_REFRESH = 1 << 0;  // download the active calendar now
const EventBits_t EV_REBUILD = 1 << 1;  // re-expand the active calendar now
const EventBits_t EV_ALL = EV_REFRESH | EV_REBUILD;

SemaphoreHandle_t s_mutex = nullptr;
EventGroupHandle_t s_events = nullptr;
Mode s_mode = Mode::Stopped;
int64_t s_openUntil = 0;
String s_openReason;
String s_lastDownload = "none yet";
int64_t s_lastDownloadAt = 0;

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

Mode currentMode() {
  Lock l;
  return s_mode;
}

bool isSchedule(Mode m) { return m == Mode::LocalSchedule || m == Mode::RemoteSchedule; }

schedule::Source sourceFor(Mode m) {
  if (m == Mode::LocalSchedule) return schedule::Source::Local;
  if (m == Mode::RemoteSchedule) return schedule::Source::Remote;
  return schedule::Source::None;
}

int64_t nowUtc() { return static_cast<int64_t>(time(nullptr)); }

void setMode(Mode m, const char* why) {
  bool changed;
  {
    Lock l;
    changed = s_mode != m;
    s_mode = m;
  }
  settings::modify([m](settings::Data& d) { d.mode = static_cast<uint8_t>(m); });
  if (changed) {
    xEventGroupSetBits(s_events, EV_REBUILD | EV_REFRESH);
    LOGI("ctl", "mode: %s (%s)", modeLabel(m), why);
    cloud::logEvent("mode", String("Način: ") + modeLabel(m) + " (" + why + ")");
  }
}

void setDownloadResult(const String& text) {
  Lock l;
  s_lastDownload = text;
  s_lastDownloadAt = nowUtc();
}

void flash(Flashes& f, const char* category, const String& text) { f.push_back(auth::Flash{category, text}); }

// Local changes are only allowed while the server cannot be reached.
bool blockedByServer(Flashes& f) {
  if (!cloud::managed()) return false;
  flash(f, "warning", "This door is managed by the server (" + settings::get().serverUrl +
                          "). Make changes there; this page only allows changes while the server is unreachable.");
  return true;
}

void noteOfflineEdit(const String& what) {
  if (cloud::adopted()) cloud::logEvent("offline_edit", "Lokalna sprememba brez povezave: " + what);
}

// The calendar download of the active mode.
bool refreshCalendar(Mode m) {
  const settings::Data cfg = settings::get();
  const bool local = m == Mode::LocalSchedule;
  const String& url = local ? cfg.localUrl : cfg.remoteCalUrl;
  if (url.isEmpty()) {
    setDownloadResult(local ? "no local calendar URL set" : "no group calendar assigned");
    return true;
  }
  const fetch::Result r = fetch::downloadCalendar(url, local ? storage::kLocalIcs : storage::kRemoteIcs);
  setDownloadResult(String(local ? "local" : "group") + ": " + (r.ok ? String("OK, ") + r.bytes + " bytes" : r.error));
  if (!r.ok) cloud::logEvent("calendar", "Prenos koledarja ni uspel: " + r.error);
  return r.ok;
}

void factoryReset() {
  LOGW("btn", "FACTORY RESET: erasing settings and calendars");
  relay::set(false);
  settings::factoryReset();
  storage::wipe();
  delay(300);
  ESP.restart();
}

// BOOT button: short press = setup network for 10 min,
// hold 10 s = default local password, hold 30 s = factory reset.
void handleButton() {
  static uint32_t pressedAt = 0;
  static uint8_t stage = 0;
  if (digitalRead(PIN_BUTTON) != LOW) {
    if (pressedAt) {
      const uint32_t held = millis() - pressedAt;
      if (stage == 0 && held >= 50 && held <= BUTTON_SHORT_PRESS_MAX_MS) {
        netmgr::openSetupAp();
      } else if (stage == 0 && held > BUTTON_SHORT_PRESS_MAX_MS) {
        LOGI("btn", "released before 10 s, nothing done");
      }
    }
    pressedAt = 0;
    stage = 0;
    return;
  }
  const uint32_t now = millis();
  if (!pressedAt) {
    pressedAt = now ? now : 1;
    return;
  }
  const uint32_t held = now - pressedAt;
  if (stage < 1 && held >= BUTTON_PASSWORD_RESET_HOLD_MS) {
    stage = 1;
    auth::resetPasswordToDefault();
    LOGW("btn", "local admin password reset to the default (keep holding 20 s more for a factory reset)");
  }
  if (stage < 2 && held >= BUTTON_FACTORY_RESET_HOLD_MS) {
    stage = 2;
    factoryReset();
  }
}

void controlTask(void*) {
  wdt::subscribe();
  bool lastOn = false;
  String lastWhy = "-";
  uint32_t lastPortalTick = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    wdt::feed();
    handleButton();
    if (millis() - lastPortalTick >= 1000) {
      lastPortalTick = millis();
      portal::tick();  // disconnect phones that unlocked / idle (gym firmware logic)
    }

    const Mode m = currentMode();
    const bool timeOk = netmgr::timeValid();
    const int64_t now = nowUtc();
    int64_t openUntil;
    {
      Lock l;
      openUntil = s_openUntil;
    }
    bool on = false;
    String why;
    if (timeOk && openUntil > now) {
      on = true;
      why = "odklep s strežnika";
    }
    if (m == Mode::ManualOn) {
      on = true;
      if (why.isEmpty()) why = "ročno odprto (OPEN)";
    } else if (m == Mode::Stopped) {
      if (portal::unlocked()) portal::lockNow();  // LOCK also ends a running PIN unlock
    } else {
      ical::Occurrence occ;
      if (timeOk && schedule::activeAt(sourceFor(m), now, portal::enabled(), &occ)) {
        on = true;
        if (why.isEmpty()) why = String("dogodek: ") + occ.summary;
      }
      if (portal::unlocked()) {
        on = true;
        if (why.isEmpty()) why = "PIN";
      }
    }
    relay::set(on);

    if (on != lastOn || why != lastWhy) {
      if (on) {
        LOGI("door", "OPEN - %s", why.c_str());
      } else if (lastOn || lastWhy == "-") {
        LOGI("door", "LOCKED");
      }
      {
        Lock l;
        s_openReason = why;
      }
      lastOn = on;
      lastWhy = why;
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_LOOP_MS));
  }
}

void netTask(void*) {
  wdt::subscribe();
  bool refreshDue = true, rebuildDue = true;
  bool timeWasValid = false;
  uint32_t nextRefresh = 0, nextRebuild = 0;
  for (;;) {
    wdt::feed();
    netmgr::tick();
    cloud::tick();  // may apply new settings (sets EV_* bits) or install firmware
    const EventBits_t bits = xEventGroupWaitBits(s_events, EV_ALL, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
    refreshDue |= (bits & EV_REFRESH) != 0;
    rebuildDue |= (bits & EV_REBUILD) != 0;

    const bool online = netmgr::online();
    const bool timeOk = netmgr::timeValid();
    if (timeOk && !timeWasValid) {
      timeWasValid = true;
      rebuildDue = true;
      LOGI("time", "clock synchronized: %s", formatTime(nowUtc()).c_str());
    }

    const Mode m = currentMode();
    if (online && isSchedule(m) && (refreshDue || static_cast<int32_t>(millis() - nextRefresh) >= 0)) {
      refreshDue = false;
      const bool ok = refreshCalendar(m);
      nextRefresh = millis() + (ok ? settings::get().calS * 1000UL : NET_RETRY_AFTER_FAILURE_MS);
      rebuildDue = true;
    }

    if (timeOk && (rebuildDue || static_cast<int32_t>(millis() - nextRebuild) >= 0)) {
      rebuildDue = false;
      const int64_t keepEnded = static_cast<int64_t>(settings::get().pinAfterMin) * 60;
      schedule::rebuild(sourceFor(currentMode()), nowUtc(), keepEnded);
      nextRebuild = millis() + SCHEDULE_REBUILD_INTERVAL_MS;
    }
  }
}

}  // namespace

const char* modeLabel(Mode m) {
  switch (m) {
    case Mode::ManualOn: return "Running";
    case Mode::LocalSchedule: return "Running in schedule mode";
    case Mode::RemoteSchedule: return "Running in remote schedule mode";
    default: return "Stopped";
  }
}

const char* statusName(int s) {
  switch (s) {
    case 0: return "LOCK";
    case 1: return "OPEN";
    case 2: return "SCHEDULE REMOTE";
    case 3: return "SCHEDULE LOCAL";
    default: return "-";
  }
}

String formatTime(int64_t utc) {
  if (utc <= 0) return "-";
  const civil::DateTime t = civil::fromSeconds(tz::utcToLocal(tz::deviceRule(), utc));
  char buf[24];
  snprintf(buf, sizeof(buf), "%02u.%02u.%04d %02u:%02u", t.d, t.mo, t.y, t.h, t.mi);
  return String(buf);
}

void begin() {
  s_mutex = xSemaphoreCreateMutex();
  s_events = xEventGroupCreate();
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  const settings::Data cfg = settings::get();
  Mode m = cfg.mode <= 3 ? static_cast<Mode>(cfg.mode) : Mode::Stopped;
  if (m == Mode::ManualOn && !RESTORE_MANUAL_ON_AT_BOOT) {
    // A door should not come back open by itself after a reboot; if the
    // server still says OPEN, the first contact opens it again.
    m = Mode::Stopped;
    LOGI("ctl", "manual ON is not restored after a reboot (the server decides)");
  }
  s_mode = m;
  settings::modify([m](settings::Data& d) { d.mode = static_cast<uint8_t>(m); });
  LOGI("ctl", "start mode: %s, device name '%s'", modeLabel(m), cfg.deviceName.c_str());

  xTaskCreatePinnedToCore(controlTask, "control", CONTROL_TASK_STACK, nullptr, CONTROL_TASK_PRIO, nullptr, APP_CPU);
  xTaskCreatePinnedToCore(netTask, "net", NET_TASK_STACK, nullptr, NET_TASK_PRIO, nullptr, APP_CPU);
}

// ------------------------------------------------------------- server -----

void applyServerStatus(int status) {
  static const Mode kModes[] = {Mode::Stopped, Mode::ManualOn, Mode::RemoteSchedule, Mode::LocalSchedule};
  if (status < 0 || status > 3) return;
  char why[32];
  snprintf(why, sizeof(why), "strežnik: %s", statusName(status));
  setMode(kModes[status], why);
}

void setOpenUntil(int64_t utc) {
  Lock l;
  if (utc != s_openUntil && utc > nowUtc()) LOGI("ctl", "server unlock until %s", formatTime(utc).c_str());
  s_openUntil = utc;
}

void requestCalendarRefresh() { xEventGroupSetBits(s_events, EV_REFRESH | EV_REBUILD); }

bool pinsBlocked() { return currentMode() == Mode::Stopped; }

schedule::Source bookingSource() { return sourceFor(currentMode()); }

// ------------------------------------------------------------ actions -----

void actionStart(Flashes& f) {
  if (blockedByServer(f)) return;
  if (currentMode() == Mode::ManualOn) {
    flash(f, "warning", "Script is already running!");
    return;
  }
  setMode(Mode::ManualOn, "lokalno: Start");
  noteOfflineEdit("Start");
  flash(f, "success", "Script started successfully!");
}

void actionStop(Flashes& f) {
  if (blockedByServer(f)) return;
  if (currentMode() == Mode::Stopped) {
    flash(f, "warning", "Script is not running!");
    return;
  }
  setMode(Mode::Stopped, "lokalno: Stop");
  noteOfflineEdit("Stop");
  flash(f, "success", "Script stopped successfully!");
}

void actionLocalSchedule(Flashes& f) {
  if (blockedByServer(f)) return;
  setMode(Mode::LocalSchedule, "lokalno: Local Schedule");
  noteOfflineEdit("Local Schedule");
  xEventGroupSetBits(s_events, EV_REFRESH | EV_REBUILD);
  if (settings::get().localUrl.isEmpty()) {
    flash(f, "warning", "Local schedule enabled, but no Local Calendar URL is set: the door stays locked.");
  } else {
    flash(f, "success", "Local schedule enabled.");
  }
}

void actionRemoteSchedule(Flashes& f) {
  if (blockedByServer(f)) return;
  setMode(Mode::RemoteSchedule, "lokalno: Remote Schedule");
  noteOfflineEdit("Remote Schedule");
  xEventGroupSetBits(s_events, EV_REFRESH | EV_REBUILD);
  flash(f, "success", settings::get().remoteCalUrl.isEmpty()
                          ? "Remote schedule enabled, but the server has not assigned a group calendar yet."
                          : "Remote schedule enabled (last group calendar received from the server).");
}

void actionUpdateLocalUrl(const String& urlIn, Flashes& f) {
  if (blockedByServer(f)) return;
  String url = urlIn;
  url.trim();
  if (url.isEmpty()) {
    flash(f, "danger", "Invalid calendar URL or unable to fetch the file.");
    return;
  }
  const fetch::Result r = fetch::downloadCalendar(url, storage::kLocalIcs);
  if (!r.ok) {
    flash(f, "danger", r.httpCode == 200 ? r.error : "Invalid calendar URL or unable to fetch the file. (" + r.error + ")");
    return;
  }
  settings::modify([&](settings::Data& d) { d.localUrl = url; });
  noteOfflineEdit("lokalni koledar");
  setDownloadResult(String("local: OK, ") + r.bytes + " bytes");
  xEventGroupSetBits(s_events, EV_REBUILD);
  flash(f, "success", "Calendar script updated and calendar file validated successfully!");
}

void actionSync(Flashes& f) {
  const settings::Data cfg = settings::get();
  const bool remote = currentMode() == Mode::RemoteSchedule;
  const String url = remote ? cfg.remoteCalUrl : cfg.localUrl;
  if (url.isEmpty()) {
    flash(f, "warning", remote ? "No group calendar assigned." : "No Local Calendar URL set.");
    return;
  }
  // Re-downloading is harmless, so it is allowed even while the server manages the door.
  const fetch::Result r = fetch::downloadCalendar(url, remote ? storage::kRemoteIcs : storage::kLocalIcs);
  setDownloadResult(String(remote ? "group" : "local") + ": " + (r.ok ? String("OK, ") + r.bytes + " bytes" : r.error));
  if (!r.ok) {
    flash(f, "danger", "Sync failed: " + r.error);
    return;
  }
  xEventGroupSetBits(s_events, EV_REBUILD);
  flash(f, "success", "Calendar synced successfully!");
}

void actionSaveSettings(const String& nameIn, const String ssid[], const String pass[], Flashes& f) {
  if (blockedByServer(f)) return;
  String name = nameIn;
  name.trim();
  bool nameOk = !name.isEmpty() && name.length() <= 32;
  for (size_t i = 0; i < name.length() && nameOk; ++i) {
    const char c = name[i];
    nameOk = isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
  }
  if (!nameOk) {
    flash(f, "danger", "Device name: 1-32 letters, digits, - or _ (e.g. V1).");
    return;
  }
  const settings::Data cfg = settings::get();
  bool netChanged = false;
  for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
    if (pass[i].length() > 0 && pass[i].length() < 8) {
      flash(f, "danger", "Wi-Fi password for network " + String(i + 1) + " must have at least 8 characters.");
      return;
    }
  }
  settings::modify([&](settings::Data& d) {
    if (name != d.deviceName) {
      d.deviceName = name;
      netChanged = true;
    }
    for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
      String s = ssid[i];
      s.trim();
      // Empty password field = keep the stored one, unless the SSID changed.
      if (s != d.wifi[i].ssid || pass[i].length()) {
        d.wifi[i].ssid = s;
        d.wifi[i].pass = s.isEmpty() ? String() : pass[i];
        netChanged = true;
      }
    }
  });
  if (netChanged) {
    netmgr::requestReconfigure();
    noteOfflineEdit("ime naprave / Wi-Fi");
  }
  (void)cfg;
  flash(f, "success", netChanged ? "Settings saved. Wi-Fi reconnects now; the page may be unreachable for a moment."
                                 : "Nothing changed.");
}

void actionServerUrl(const String& urlIn, Flashes& f) {
  String url = urlIn;
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (!url.isEmpty() && !url.startsWith("https://") && !url.startsWith("http://")) {
    flash(f, "danger", "The server URL must start with https://");
    return;
  }
  const settings::Data cfg = settings::get();
  if (url == cfg.serverUrl) {
    flash(f, "info", "Server URL unchanged.");
    return;
  }
  // A different server does not know this door's key: start over as a new door there.
  settings::modify([&](settings::Data& d) {
    d.serverUrl = url;
    d.token = "";
  });
  cloud::requestSync();
  LOGI("cloud", "server URL changed to %s", url.c_str());
  flash(f, "success", url.isEmpty() ? "Server disabled: the door now runs on its local settings only."
                                    : "Server URL saved. The door registers there as a new door (pairing code " +
                                          cfg.pairCode + ").");
}

Status status() {
  Status s;
  {
    Lock l;
    s.mode = s_mode;
    s.openReason = s_openReason;
    s.openUntil = s_openUntil;
    s.lastDownload = s_lastDownload;
    s.lastDownloadAt = s_lastDownloadAt;
  }
  s.serverStatus = settings::get().serverStatus;
  s.relayOn = relay::get();
  s.timeValid = netmgr::timeValid();
  s.nowUtc = nowUtc();
  s.sched = schedule::info();
  if (s.timeValid && isSchedule(s.mode)) {
    ical::Occurrence o;
    if (schedule::activeAt(sourceFor(s.mode), s.nowUtc, portal::enabled(), &o)) {
      s.eventActive = true;
      s.eventSummary = o.summary;
      s.eventEnd = o.end;
    }
    if (schedule::nextAfter(sourceFor(s.mode), s.nowUtc, o)) {
      s.hasNext = true;
      s.nextSummary = o.summary;
      s.nextStart = o.start;
    }
  }
  return s;
}

}  // namespace ctl
