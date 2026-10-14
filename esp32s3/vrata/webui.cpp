#include "webui.h"

#include <Update.h>
#include <WebServer.h>

#include <functional>

#include "applog.h"
#include "auth.h"
#include "cloud.h"
#include "config.h"
#include "controller.h"
#include "netmgr.h"
#include "portal.h"
#include "settings.h"
#include "src/generated/bootstrap_css.h"
#include "wdt.h"

namespace webui {

namespace {

WebServer s_server(WEB_PORT);
bool s_otaAllowed = false;
String s_otaError;

// ------------------------------------------------------------ helpers -----

String esc(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

String jsonStr(const String& s) {
  String o = "\"";
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o + "\"";
}

String head(const char* title) {
  String h;
  h.reserve(4096);
  h += F("<!DOCTYPE html><html lang=\"sl\"><head><meta charset=\"UTF-8\">"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>");
  h += title;
  h += F("</title><link rel=\"stylesheet\" href=\"/static/bootstrap.min.css\"></head><body>"
         "<div class=\"container mt-5\">");
  return h;
}

const char* kFoot = "</div></body></html>";

void sendHtml(int code, const String& html) {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.sendHeader("X-Frame-Options", "DENY");
  s_server.sendHeader("X-Content-Type-Options", "nosniff");
  s_server.sendHeader("Referrer-Policy", "no-referrer");
  s_server.send(code, "text/html; charset=utf-8", html);
}

void redirect(const String& to) {
  s_server.sendHeader("Location", to, true);
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(303, "text/plain", "");
}

auth::Session* session() { return auth::sessionFromCookie(s_server.header("Cookie")); }

// Flask-Login's @login_required
auth::Session* requireLogin() {
  auth::Session* s = session();
  if (!s) redirect("/login");
  return s;
}

bool csrfOk(const auth::Session* s) { return s && s_server.arg("csrf_token") == s->csrf; }

String csrfField(const auth::Session* s) {
  return String("<input type=\"hidden\" name=\"csrf_token\" value=\"") + s->csrf + "\">";
}

String alerts(std::vector<auth::Flash>& flashes) {
  String h;
  for (const auth::Flash& f : flashes) {
    h += "<div class=\"alert alert-" + f.category + "\" role=\"alert\">" + esc(f.text) + "</div>";
  }
  flashes.clear();
  return h;
}

String dis(bool disabled) { return disabled ? " disabled" : ""; }

// The control panel is never reachable from the open PIN-portal network.
std::function<void()> guarded(void (*fn)()) {
  return [fn]() {
    if (portal::isPortalConnection(s_server.client().localIP())) {
      s_server.send(403, "text/plain", "Not available on this network.");
      return;
    }
    fn();
  };
}

// --------------------------------------------------------- login page -----

void renderLogin(const String& message, const char* category) {
  String h = head("Login");
  h += F("<h1>Login</h1>");
  if (message.length()) h += String("<div class=\"alert alert-") + category + "\" role=\"alert\">" + esc(message) + "</div>";
  h += F("<form method=\"POST\" action=\"/login\">"
         "<div class=\"form-group\"><label for=\"username\">Username</label> "
         "<input id=\"username\" name=\"username\" required type=\"text\" value=\"\" autocomplete=\"username\"></div>"
         "<div class=\"form-group\"><label for=\"password\">Password</label> "
         "<input id=\"password\" name=\"password\" required type=\"password\" value=\"\" "
         "autocomplete=\"current-password\"></div>"
         "<div class=\"form-group\"><input id=\"submit\" name=\"submit\" type=\"submit\" value=\"Login\"></div>"
         "</form>");
  h += kFoot;
  sendHtml(200, h);
}

void handleLoginGet() {
  if (session()) {
    redirect("/");
    return;
  }
  if (s_server.arg("m") == "pw") {
    renderLogin("Password changed successfully! Please log in with your new password.", "success");
  } else {
    renderLogin("", "info");
  }
}

void handleLoginPost() {
  uint32_t wait;
  if (auth::lockedOut(wait)) {
    renderLogin("Too many failed attempts. Try again in " + String(wait) + " s.", "danger");
    return;
  }
  const String user = s_server.arg("username");
  const String pass = s_server.arg("password");
  // Same validators as the WTForms LoginForm: username 2-20, password 8-20.
  const bool formOk = user.length() >= 2 && user.length() <= 20 && pass.length() >= 8 && pass.length() <= 20;
  if (formOk && auth::checkLogin(user, pass)) {
    auth::noteSuccess();
    auth::Session* s = auth::createSession();
    s_server.sendHeader("Set-Cookie", String("vrata_session=") + s->token + "; Path=/; HttpOnly; SameSite=Strict");
    LOGI("web", "login from %s", s_server.client().remoteIP().toString().c_str());
    redirect("/");
    return;
  }
  auth::noteFailure();
  LOGW("web", "failed login from %s", s_server.client().remoteIP().toString().c_str());
  renderLogin("Invalid username or password", "danger");
}

void handleLogout() {
  auth::Session* s = session();
  if (s) auth::endSession(s);
  s_server.sendHeader("Set-Cookie", "vrata_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  redirect("/login");
}

// ------------------------------------------------ change password page -----

void renderChangePassword(auth::Session* s) {
  String h = head("Change Password");
  h += F("<h1>Change Password</h1>");
  h += alerts(s->flashes);
  h += F("<form method=\"POST\" action=\"/change_password\">");
  h += csrfField(s);
  h += F("<div class=\"form-group\"><label for=\"old_password\">Old Password</label> "
         "<input id=\"old_password\" name=\"old_password\" required type=\"password\" value=\"\"></div>"
         "<div class=\"form-group\"><label for=\"new_password\">New Password</label> "
         "<input id=\"new_password\" name=\"new_password\" required type=\"password\" value=\"\"></div>"
         "<div class=\"form-group\"><label for=\"confirm_password\">Confirm New Password</label> "
         "<input id=\"confirm_password\" name=\"confirm_password\" required type=\"password\" value=\"\"></div>"
         "<div class=\"form-group\"><input id=\"submit\" name=\"submit\" type=\"submit\" value=\"Change Password\">"
         "</div></form><a href=\"/\">Back</a>");
  h += kFoot;
  sendHtml(200, h);
}

void handleChangePasswordGet() {
  auth::Session* s = requireLogin();
  if (s) renderChangePassword(s);
}

void handleChangePasswordPost() {
  auth::Session* s = requireLogin();
  if (!s) return;
  if (!csrfOk(s)) {
    s->flashes.push_back({"danger", "Session expired, please try again."});
    renderChangePassword(s);
    return;
  }
  const String oldPw = s_server.arg("old_password");
  const String newPw = s_server.arg("new_password");
  const String confirm = s_server.arg("confirm_password");
  if (newPw.length() < 8 || newPw.length() > 20) {
    s->flashes.push_back({"danger", "New password must be 8-20 characters long."});
  } else if (!auth::checkPassword(oldPw)) {
    s->flashes.push_back({"danger", "Old password is incorrect."});
  } else if (newPw != confirm) {
    s->flashes.push_back({"danger", "New password and confirmation do not match."});
  } else {
    auth::setPassword(newPw);
    LOGI("web", "admin password changed");
    auth::endSession(s);  // app.py: logout_user() + redirect to login
    s_server.sendHeader("Set-Cookie", "vrata_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    redirect("/login?m=pw");
    return;
  }
  renderChangePassword(s);
}

// ----------------------------------------------------------- index page -----

String row(const char* label, const String& valueHtml) {
  return String("<tr><th scope=\"row\" style=\"width:35%\">") + label + "</th><td>" + valueHtml + "</td></tr>";
}

void handleIndexGet() {
  auth::Session* s = requireLogin();
  if (!s) return;
  const settings::Data cfg = settings::get();
  const ctl::Status st = ctl::status();
  const netmgr::Info net = netmgr::info();
  const cloud::Info cl = cloud::info();
  const portal::Info pi = portal::info();
  const bool managed = cl.managed;

  String h = head("Control Panel");
  h.reserve(14000);
  h += "<h1>Control Panel - " + esc(cfg.deviceName) + "</h1>";
  h += alerts(s->flashes);
  if (managed) {
    h += "<div class=\"alert alert-info\">This door is managed by <b>" + esc(cfg.serverUrl) +
         "</b>. Settings are changed there. This page becomes editable when the server has been unreachable for "
         "a few minutes (for example when the internet is down).</div>";
  }

  // --- local calendar (was the "Local Calendar URL" card) ---
  h += F("<div class=\"card\"><div class=\"card-body\"><form method=\"POST\" action=\"/\">");
  h += csrfField(s);
  h += "<div class=\"form-group\"><label for=\"calendar_url\">Local Calendar URL:</label>"
       "<input type=\"text\" name=\"calendar_url\" class=\"form-control\" id=\"calendar_url\" value=\"" +
       esc(cfg.localUrl) + "\"" + dis(managed) + "></div>";
  h += "<button type=\"submit\" name=\"action\" value=\"update\" class=\"btn btn-primary\"" + dis(managed) +
       ">Update</button> <button type=\"submit\" name=\"action\" value=\"sync\" class=\"btn btn-success\">Sync</button>";
  h += F("</form></div></div>");

  // --- Script Status + buttons ---
  const char* badge = st.mode == ctl::Mode::ManualOn ? "badge-success"
                      : st.mode == ctl::Mode::Stopped ? "badge-danger"
                                                      : "badge-info";
  h += String("<hr><h4>Script Status: <span class=\"badge ") + badge + "\">" + ctl::modeLabel(st.mode) + "</span></h4>";
  h += F("<form method=\"POST\" action=\"/\">");
  h += csrfField(s);
  h += "<button type=\"submit\" name=\"action\" value=\"start\" class=\"btn btn-success\"" + dis(managed) + ">Start</button> "
       "<button type=\"submit\" name=\"action\" value=\"stop\" class=\"btn btn-danger\"" + dis(managed) + ">Stop</button> "
       "<button type=\"submit\" name=\"action\" value=\"schedule\" class=\"btn btn-info\"" + dis(managed) + ">Local Schedule</button> "
       "<button type=\"submit\" name=\"action\" value=\"remote_schedule\" class=\"btn btn-info\"" + dis(managed) +
       ">Remote Schedule</button></form>";

  // --- status details ---
  h += F("<div class=\"card mt-4\"><div class=\"card-body\"><h5 class=\"card-title\">Status</h5>"
         "<table class=\"table table-sm mb-0\">");
  h += row("Door", (st.relayOn ? String("<span class=\"badge badge-success\">OPEN</span> ") + esc(st.openReason)
                               : String("<span class=\"badge badge-secondary\">LOCKED</span>")));
  String server = esc(cl.state);
  if (cl.lastOkAt) server += " &ndash; last contact " + ctl::formatTime(cl.lastOkAt);
  if (!cl.lastError.isEmpty()) server += "<br><small class=\"text-danger\">" + esc(cl.lastError) + "</small>";
  if (!cl.adopted) server += "<br>Pairing code for the server: <b><code>" + esc(cl.pairCode) + "</code></b>";
  h += row("Server", server);
  if (st.serverStatus >= 0) h += row("Server status", String(st.serverStatus) + " " + ctl::statusName(st.serverStatus));
  if (st.openUntil > st.nowUtc && st.timeValid) h += row("Unlocked by server", "until " + ctl::formatTime(st.openUntil));
  if (st.mode == ctl::Mode::LocalSchedule || st.mode == ctl::Mode::RemoteSchedule) {
    if (!st.timeValid) {
      h += row("Calendar", "<span class=\"text-danger\">waiting for the clock (NTP) &ndash; door stays locked</span>");
    } else {
      h += row("Current event", st.eventActive ? esc(st.eventSummary) + " until " + ctl::formatTime(st.eventEnd) : "none");
      h += row("Next event", st.hasNext ? esc(st.nextSummary) + " at " + ctl::formatTime(st.nextStart) : "none in the next 10 days");
      String cal = String(schedule::sourceName(st.sched.source)) + ": ";
      if (st.sched.ok) {
        cal += String(st.sched.occurrences) + " upcoming (" + st.sched.bookings + " bookings), " + st.sched.events +
               " events in file";
        if (st.sched.skipped) cal += String(", ") + st.sched.skipped + " skipped (see log)";
      } else {
        cal += esc(st.sched.error);
      }
      h += row("Calendar", cal);
    }
  }
  h += row("Last download", esc(st.lastDownload) + " (" + ctl::formatTime(st.lastDownloadAt) + ")");
  if (pi.enabled) {
    String p = "network <b>" + esc(pi.ssid) + "</b> " + (pi.apUp ? "on" : "<span class=\"text-warning\">off</span>") +
               ", phones: " + pi.phones + ", permanent users: " + pi.users;
    if (pi.unlockLeftS) p += ", <b>unlocked for " + String(pi.unlockLeftS) + " s</b>";
    if (pi.pausedLeftS) p += ", network off for " + String(pi.pausedLeftS) + " s after the unlock";
    h += row("PIN portal", p);
  }
  h += row("Clock", st.timeValid ? ctl::formatTime(st.nowUtc) : String("not synchronized"));
  String wifi = net.staConnected ? esc(net.ssid) + ", " + net.ip + ", " + net.rssi + " dBm" : String("not connected");
  if (net.apMode == "setup") wifi += "<br>setup network " + esc(net.apSsid) + " (" + net.apIp + ")";
  if (net.wifiTrial) wifi += "<br><span class=\"text-warning\">trying new Wi-Fi settings from the server</span>";
  h += row("Wi-Fi", wifi);
  String fw = String(FW_NAME " " FW_VERSION ", up ") + (millis() / 3600000UL) + " h, free heap " + (ESP.getFreeHeap() / 1024) + " KB";
  if (cl.otaPendingVerify) fw += "<br><span class=\"text-warning\">new firmware, not confirmed yet</span>";
  if (!cl.otaError.isEmpty()) fw += "<br><span class=\"text-danger\">" + esc(cl.otaError) + "</span>";
  h += row("Firmware", fw);
  h += F("</table></div></div>");

  // --- server link (always editable for the local admin) ---
  h += F("<div class=\"card mt-3\"><div class=\"card-body\"><h5 class=\"card-title\">Server</h5><form method=\"POST\" action=\"/\">");
  h += csrfField(s);
  h += "<div class=\"form-group\"><label for=\"server_url\">Server URL (empty = run on local settings only):</label>"
       "<input type=\"text\" name=\"server_url\" id=\"server_url\" class=\"form-control\" value=\"" + esc(cfg.serverUrl) +
       "\"></div><button type=\"submit\" name=\"action\" value=\"server\" class=\"btn btn-secondary\">Save</button>"
       "<p class=\"small text-muted mt-2 mb-0\">After a change the door registers at that server as a new door and must "
       "be adopted there (pairing code " + esc(cl.pairCode) + ").</p></form></div></div>";

  // --- settings (name + Wi-Fi) ---
  h += F("<div class=\"card mt-3\"><div class=\"card-body\"><h5 class=\"card-title\">Settings</h5>"
         "<form method=\"POST\" action=\"/\">");
  h += csrfField(s);
  h += "<div class=\"form-group\"><label for=\"device_name\">Device name (V1, V2 ...):</label>"
       "<input type=\"text\" name=\"device_name\" id=\"device_name\" class=\"form-control\" maxlength=\"32\" value=\"" +
       esc(cfg.deviceName) + "\"" + dis(managed) + "></div>";
  for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
    const String n(i + 1);
    h += "<div class=\"form-row\"><div class=\"form-group col-md-6\"><label for=\"wifi_ssid" + n + "\">Wi-Fi " + n +
         " name (SSID)</label><input type=\"text\" name=\"wifi_ssid" + n + "\" id=\"wifi_ssid" + n +
         "\" class=\"form-control\" value=\"" + esc(cfg.wifi[i].ssid) + "\"" + dis(managed) + "></div>";
    h += "<div class=\"form-group col-md-6\"><label for=\"wifi_pass" + n + "\">Wi-Fi " + n +
         " password</label><input type=\"password\" name=\"wifi_pass" + n + "\" id=\"wifi_pass" + n +
         "\" class=\"form-control\" placeholder=\"" + (cfg.wifi[i].pass.length() ? "(unchanged)" : "") + "\"" +
         dis(managed) + "></div></div>";
  }
  h += "<p class=\"small text-muted\">An empty password field keeps the stored password. When a network name is changed, "
       "the password entered with it is used (empty = open network). The strongest known network is joined. "
       "A short press on BOOT opens the setup network for 10 minutes.</p><button type=\"submit\" name=\"action\" "
       "value=\"settings\" class=\"btn btn-secondary\"" + dis(managed) + ">Save settings</button></form></div></div>";

  h += F("<p class=\"mt-3\"><a href=\"/change_password\">Change Password</a><br><a href=\"/logout\">Logout</a><br>"
         "<a href=\"/log\">Log</a>");
#if ENABLE_WEB_OTA
  h += F("<br><a href=\"/update\">Firmware update</a>");
#endif
  h += F("</p>");
  h += kFoot;
  sendHtml(200, h);
}

void handleIndexPost() {
  auth::Session* s = requireLogin();
  if (!s) return;
  if (!csrfOk(s)) {
    s->flashes.push_back({"danger", "Session expired, please try again."});
    redirect("/");
    return;
  }
  const String action = s_server.arg("action");
  LOGI("web", "action '%s'", action.c_str());
  ctl::Flashes& f = s->flashes;
  if (action == "start") {
    ctl::actionStart(f);
  } else if (action == "stop") {
    ctl::actionStop(f);
  } else if (action == "schedule") {
    ctl::actionLocalSchedule(f);
  } else if (action == "remote_schedule") {
    ctl::actionRemoteSchedule(f);
  } else if (action == "update") {
    ctl::actionUpdateLocalUrl(s_server.arg("calendar_url"), f);
  } else if (action == "sync") {
    ctl::actionSync(f);
  } else if (action == "server") {
    ctl::actionServerUrl(s_server.arg("server_url"), f);
  } else if (action == "settings") {
    String ssid[WIFI_MAX_NETWORKS], pass[WIFI_MAX_NETWORKS];
    for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
      ssid[i] = s_server.arg("wifi_ssid" + String(i + 1));
      pass[i] = s_server.arg("wifi_pass" + String(i + 1));
    }
    ctl::actionSaveSettings(s_server.arg("device_name"), ssid, pass, f);
  }
  redirect("/");  // post/redirect/get, like app.py
}

// ------------------------------------------------------ misc endpoints -----

void handleCss() {
  s_server.sendHeader("Content-Encoding", "gzip");
  s_server.sendHeader("Cache-Control", "public, max-age=604800");
  s_server.send_P(200, "text/css", reinterpret_cast<const char*>(bootstrap_css_gz), bootstrap_css_gz_len);
}

void handleLog() {
  if (!requireLogin()) return;
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "text/plain; charset=utf-8", applog::dump());
}

void handleApiStatus() {
  if (!requireLogin()) return;
  const ctl::Status st = ctl::status();
  const settings::Data cfg = settings::get();
  const netmgr::Info net = netmgr::info();
  const cloud::Info cl = cloud::info();
  const portal::Info pi = portal::info();
  String j = "{";
  j += "\"device\":" + jsonStr(cfg.deviceName);
  j += ",\"mode\":" + String(static_cast<int>(st.mode));
  j += ",\"mode_label\":" + jsonStr(ctl::modeLabel(st.mode));
  j += ",\"server_status\":" + String(st.serverStatus);
  j += ",\"door_open\":" + String(st.relayOn ? "true" : "false");
  j += ",\"open_reason\":" + jsonStr(st.openReason);
  j += ",\"server_state\":" + jsonStr(cl.state);
  j += ",\"managed\":" + String(cl.managed ? "true" : "false");
  j += ",\"adopted\":" + String(cl.adopted ? "true" : "false");
  j += ",\"time_valid\":" + String(st.timeValid ? "true" : "false");
  j += ",\"now\":" + String(static_cast<long>(st.nowUtc));
  j += ",\"event_active\":" + String(st.eventActive ? "true" : "false");
  j += ",\"event\":" + jsonStr(st.eventSummary);
  j += ",\"next_event\":" + jsonStr(st.nextSummary);
  j += ",\"calendar_source\":" + jsonStr(schedule::sourceName(st.sched.source));
  j += ",\"calendar_ok\":" + String(st.sched.ok ? "true" : "false");
  j += ",\"occurrences\":" + String(st.sched.occurrences);
  j += ",\"bookings\":" + String(st.sched.bookings);
  j += ",\"last_download\":" + jsonStr(st.lastDownload);
  j += ",\"portal_enabled\":" + String(pi.enabled ? "true" : "false");
  j += ",\"portal_phones\":" + String(pi.phones);
  j += ",\"portal_unlock_left\":" + String(pi.unlockLeftS);
  j += ",\"wifi_ssid\":" + jsonStr(net.ssid);
  j += ",\"ip\":" + jsonStr(net.ip);
  j += ",\"rssi\":" + String(net.rssi);
  j += ",\"own_network\":" + jsonStr(net.apMode);
  j += ",\"free_heap\":" + String(ESP.getFreeHeap());
  j += ",\"uptime_s\":" + String(millis() / 1000UL);
  j += ",\"firmware\":" + jsonStr(FW_VERSION);
  j += "}";
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "application/json", j);
}

// Unknown paths and captive-portal probes land on the control panel.
void handleNotFound() {
  if (portal::isPortalConnection(s_server.client().localIP())) {
    s_server.send(403, "text/plain", "Not available on this network.");
    return;
  }
  redirect("http://" + s_server.client().localIP().toString() + ":" + String(WEB_PORT) + "/");
}

#if ENABLE_WEB_OTA
void handleUpdateGet() {
  auth::Session* s = requireLogin();
  if (!s) return;
  String h = head("Firmware update");
  h += F("<h1>Firmware update</h1><p>Upload <code>vrata.ino.bin</code> (Arduino IDE: Sketch &rarr; Export Compiled "
         "Binary). The door locks for a few seconds while the board restarts; settings and calendars are kept. "
         "Doors managed by the server are normally updated from there.</p>");
  h += String("<form method=\"POST\" action=\"/update?csrf_token=") + s->csrf +
       "\" enctype=\"multipart/form-data\"><div class=\"form-group\"><input type=\"file\" name=\"firmware\" "
       "accept=\".bin\" required></div><button type=\"submit\" class=\"btn btn-warning\">Upload and restart</button>"
       "</form><p class=\"mt-3\"><a href=\"/\">Back</a></p>";
  h += kFoot;
  sendHtml(200, h);
}

void handleUpdateUpload() {
  if (portal::isPortalConnection(s_server.client().localIP())) return;
  HTTPUpload& up = s_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    s_otaError = "";
    s_otaAllowed = csrfOk(session());
    if (!s_otaAllowed) {
      s_otaError = "not logged in";
      return;
    }
    LOGI("ota", "firmware upload started (%s)", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      s_otaError = Update.errorString();
      s_otaAllowed = false;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    wdt::feed();
    if (s_otaAllowed && Update.write(up.buf, up.currentSize) != up.currentSize) {
      s_otaError = Update.errorString();
      s_otaAllowed = false;
      Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_otaAllowed && !Update.end(true)) {
      s_otaError = Update.errorString();
      s_otaAllowed = false;
    }
    if (s_otaAllowed) LOGI("ota", "firmware upload finished: %u bytes", static_cast<unsigned>(up.totalSize));
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_otaAllowed) Update.abort();
    s_otaAllowed = false;
    s_otaError = "upload aborted";
  }
}

void handleUpdateDone() {
  if (!session()) {
    redirect("/login");
    return;
  }
  const bool ok = s_otaAllowed && s_otaError.isEmpty();
  String h = head("Firmware update");
  if (ok) {
    h += F("<div class=\"alert alert-success\">Update successful. Restarting&hellip; reload this page in 20 s.</div>");
  } else {
    h += "<div class=\"alert alert-danger\">Update failed: " + esc(s_otaError) + "</div><a href=\"/update\">Back</a>";
    LOGE("ota", "update failed: %s", s_otaError.c_str());
  }
  h += kFoot;
  sendHtml(ok ? 200 : 500, h);
  if (ok) {
    cloud::noteLocalOta();  // confirmed after 3 min without crashing, otherwise rolled back
    delay(500);
    ESP.restart();
  }
}
#endif

void webTask(void*) {
  wdt::subscribe();
  for (;;) {
    wdt::feed();
    s_server.handleClient();
    netmgr::processDns();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace

void begin() {
  static const char* headerKeys[] = {"Cookie"};
  s_server.collectHeaders(headerKeys, 1);
  s_server.on("/", HTTP_GET, guarded(handleIndexGet));
  s_server.on("/", HTTP_POST, guarded(handleIndexPost));
  s_server.on("/login", HTTP_GET, guarded(handleLoginGet));
  s_server.on("/login", HTTP_POST, guarded(handleLoginPost));
  s_server.on("/logout", HTTP_GET, guarded(handleLogout));
  s_server.on("/change_password", HTTP_GET, guarded(handleChangePasswordGet));
  s_server.on("/change_password", HTTP_POST, guarded(handleChangePasswordPost));
  s_server.on("/static/bootstrap.min.css", HTTP_GET, guarded(handleCss));
  s_server.on("/log", HTTP_GET, guarded(handleLog));
  s_server.on("/api/status", HTTP_GET, guarded(handleApiStatus));
#if ENABLE_WEB_OTA
  s_server.on("/update", HTTP_GET, guarded(handleUpdateGet));
  s_server.on("/update", HTTP_POST, guarded(handleUpdateDone), handleUpdateUpload);
#endif
  s_server.onNotFound(handleNotFound);
  s_server.begin();
  LOGI("web", "control panel on port %d", WEB_PORT);
  xTaskCreatePinnedToCore(webTask, "web", WEB_TASK_STACK, nullptr, WEB_TASK_PRIO, nullptr, APP_CPU);
}

}  // namespace webui
