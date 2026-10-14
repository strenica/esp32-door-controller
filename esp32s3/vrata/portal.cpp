#include "portal.h"

#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#include <utility>
#include <vector>

#include "applog.h"
#include "cloud.h"
#include "config.h"
#include "controller.h"
#include "ical.h"
#include "netmgr.h"
#include "schedule.h"
#include "settings.h"

namespace portal {

namespace {

httpd_handle_t s_http = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
volatile bool s_enabled = false;
uint32_t s_unlockS = 600;
uint32_t s_pauseS = 540;
int64_t s_beforeS = 900;
int64_t s_afterS = 900;
String s_ssid;
std::vector<std::pair<String, String>> s_users;  // username, PIN

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
uint64_t s_unlockUntilMs = 0;
uint64_t s_pauseFromMs = 0;   // network off from here ...
uint64_t s_pauseUntilMs = 0;  // ... until here

uint64_t nowMs() { return esp_timer_get_time() / 1000; }  // 64-bit, never rolls over

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

// ------------------------------------------------- phones on the network -----
struct Phone {
  uint8_t mac[6];
  uint32_t ip;  // from its DHCP lease, 0 until known
  bool connected;
  uint64_t activeUntil;   // disconnected when this passes without using the page
  uint64_t kickAt;        // 0, or when to disconnect it after it unlocked
  uint64_t blockedUntil;  // refused until then
  uint64_t lastKick;
};
Phone s_phones[PORTAL_MAX_CLIENTS + 4];

struct Strikes {
  uint8_t fails;
  uint8_t level;
  uint64_t until;
};
struct IpStrikes {
  uint32_t ip;
  uint64_t seen;
  Strikes s;
};
struct AccountStrikes {
  uint32_t hash;
  uint64_t seen;
  Strikes s;
};
IpStrikes s_ipStrikes[8];
AccountStrikes s_accStrikes[16];

bool isBlocked(const Strikes& s) { return nowMs() < s.until; }

void addStrike(Strikes& s, bool escalate) {
  if (++s.fails < PORTAL_MAX_FAILS) return;
  s.fails = 0;
  const uint8_t shift = escalate ? s.level : 0;  // accounts: 1, 2, 4, then 8 minutes
  if (escalate && s.level < 3) s.level++;
  s.until = nowMs() + (static_cast<uint64_t>(PORTAL_LOCKOUT_MS) << shift);
  LOGW("portal", "locked out after repeated failures");
}

Strikes& ipFor(uint32_t ip) {
  IpStrikes* oldest = &s_ipStrikes[0];
  for (IpStrikes& e : s_ipStrikes) {
    if (e.ip == ip) {
      e.seen = nowMs();
      return e.s;
    }
    if (e.seen < oldest->seen) oldest = &e;
  }
  *oldest = IpStrikes{ip, nowMs(), {}};
  return oldest->s;
}

Strikes& accountFor(const std::string& user) {
  const uint32_t h = ical::fnv1a(user);
  AccountStrikes* oldest = &s_accStrikes[0];
  for (AccountStrikes& e : s_accStrikes) {
    if (e.hash == h && e.seen) {
      e.seen = nowMs();
      return e.s;
    }
    if (e.seen < oldest->seen) oldest = &e;
  }
  *oldest = AccountStrikes{h, nowMs(), {}};
  return oldest->s;
}

String macStr(const uint8_t* m) {
  char s[18];
  snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

Phone& phoneFor(const uint8_t* mac) {
  Phone* spare = nullptr;
  for (Phone& p : s_phones) {
    if (!memcmp(p.mac, mac, 6)) return p;
    if (!p.connected && (!spare || p.blockedUntil < spare->blockedUntil)) spare = &p;
  }
  if (!spare) spare = &s_phones[0];
  *spare = Phone{};
  memcpy(spare->mac, mac, 6);
  return *spare;
}

Phone* phoneByIp(uint32_t ip) {
  for (Phone& p : s_phones) {
    if (p.connected && p.ip && p.ip == ip) return &p;
  }
  return nullptr;
}

void markActive(uint32_t ip, uint32_t ms) {
  Phone* p = phoneByIp(ip);
  if (p && nowMs() + ms > p->activeUntil) p->activeUntil = nowMs() + ms;
}

void kick(Phone& p, const char* why) {
  uint16_t aid;
  if (esp_wifi_ap_get_sta_aid(p.mac, &aid) == ESP_OK) esp_wifi_deauth_sta(aid);
  p.kickAt = 0;
  p.lastKick = nowMs();
  p.blockedUntil = nowMs() + PORTAL_COOLDOWN_MS;
  LOGI("portal", "disconnected %s: %s", macStr(p.mac).c_str(), why);
}

void checkPhones() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return;
  esp_netif_pair_mac_ip_t pairs[sizeof(list.sta) / sizeof(list.sta[0])];
  for (int i = 0; i < list.num; i++) {
    memcpy(pairs[i].mac, list.sta[i].mac, 6);
    pairs[i].ip.addr = 0;
  }
  if (list.num) esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), list.num, pairs);  // MAC -> IP

  const uint64_t now = nowMs();
  bool present[sizeof(s_phones) / sizeof(s_phones[0])] = {};
  for (int i = 0; i < list.num; i++) {
    Phone& p = phoneFor(pairs[i].mac);
    if (!p.connected) {  // just joined
      p.connected = true;
      p.activeUntil = now + PORTAL_IDLE_KICK_MS;
      p.kickAt = 0;
    }
    p.ip = pairs[i].ip.addr;
    present[&p - s_phones] = true;
  }
  for (size_t i = 0; i < sizeof(s_phones) / sizeof(s_phones[0]); i++) {
    Phone& p = s_phones[i];
    if (!present[i]) {
      p.connected = false;
      p.ip = 0;
      continue;
    }
    if (now - p.lastKick < 2000) continue;  // already kicked, give it a moment to leave
    if (now < p.blockedUntil) {
      kick(p, "rejoined during the cooldown");
    } else if (p.kickAt && now >= p.kickAt) {
      kick(p, "unlocked, done");
    } else if (now >= p.activeUntil) {
      kick(p, "joined but did not sign in");
    }
  }
}

// ---------------------------------------------------------------- page -----
const char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Odprtje vrat / Door</title>
<style>
  :root{color-scheme:dark light}
  body{margin:0;min-height:100vh;display:flex;flex-direction:column;align-items:center;
       justify-content:center;gap:14px;padding:20px;font:17px system-ui,sans-serif;
       background:#0f1115;color:#e8eaf0;text-align:center}
  @media (prefers-color-scheme:light){body{background:#f5f6fa;color:#14161c}}
  h1{font-size:22px;line-height:1.3;margin:0}
  p{margin:0;max-width:300px;font-size:15px;line-height:1.45;opacity:.85}
  input{width:240px;padding:14px;border-radius:12px;border:1px solid #8a90a0;font-size:18px;
        background:transparent;color:inherit;text-align:center}
  #b{width:240px;padding:16px;border-radius:12px;border:0;background:#5b6cff;color:#fff;
     font-size:20px;font-weight:600}
  #b:disabled{opacity:.5}
  #s{opacity:.9;min-height:48px;max-width:300px;line-height:1.45;white-space:pre-line}
  small{opacity:.6;max-width:300px;font-size:13px;line-height:1.5}
</style></head>
<body>
  <h1>Odprtje vrat<br>Door access</h1>
  <p>Vpišite ime in PIN iz rezervacije.<br>Enter the name and PIN from your booking.</p>
  <input id="u" placeholder="Ime in priimek / Full name" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
  <input id="p" placeholder="PIN" type="password" inputmode="numeric" autocomplete="off">
  <button id="b">Odpri / Open</button>
  <div id="s"></div>
<script>
const $=id=>document.getElementById(id);
$('b').onclick=async()=>{
  const b=$('b'); b.disabled=true; $('s').textContent='';
  try{
    const r=await fetch('/open',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'u='+encodeURIComponent($('u').value)+'&p='+encodeURIComponent($('p').value)});
    const j=await r.json();
    if(j.ok){
      const t=j.s>=120?Math.round(j.s/60)+' min':j.s+' s';
      $('s').textContent='Odprto za '+t+' / Unlocked for '+t+'.\nOdklop iz omrežja sledi.\nYou will be disconnected shortly.';
      $('p').value='';
    } else { $('s').textContent=j.msg; }
  }catch(e){ $('s').textContent='Ni povezave z vrati / Could not reach the door'; }
  b.disabled=false;
};
</script></body></html>)HTML";

// ------------------------------------------------------- http helpers -----
bool sockAddr(httpd_req_t* req, bool local, uint32_t& out) {
  sockaddr_in6 a = {};
  socklen_t len = sizeof(a);
  const int fd = httpd_req_to_sockfd(req);
  const int rc = local ? getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len)
                       : getpeername(fd, reinterpret_cast<sockaddr*>(&a), &len);
  if (rc != 0) return false;
  if (a.sin6_family == AF_INET) {
    out = reinterpret_cast<sockaddr_in*>(&a)->sin_addr.s_addr;
  } else {
    out = a.sin6_addr.un.u32_addr[3];  // IPv4 on the dual-stack socket (::ffff:a.b.c.d)
  }
  return true;
}

uint32_t clientIp(httpd_req_t* req) {
  uint32_t ip = 0;
  sockAddr(req, false, ip);
  return ip;
}

bool onPortal(httpd_req_t* req) {
  uint32_t local = 0;
  return netmgr::portalApActive() && sockAddr(req, true, local) &&
         local == static_cast<uint32_t>(netmgr::portalIp());
}

esp_err_t respond(httpd_req_t* req, const char* status, const char* type, const char* body, size_t len) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, len);
}

// Not the portal network: send the browser to the control panel on :5000.
esp_err_t toPanel(httpd_req_t* req) {
  uint32_t local = 0;
  sockAddr(req, true, local);
  char url[40];
  snprintf(url, sizeof(url), "http://%s:%d/", IPAddress(local).toString().c_str(), WEB_PORT);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", url);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "Control panel", HTTPD_RESP_USE_STRLEN);
}

String readForm(httpd_req_t* req) {
  if (req->content_len > 1024) return "";
  String body;
  char buf[256];
  size_t left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    const int n = httpd_req_recv(req, buf, left < sizeof(buf) ? left : sizeof(buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) return "";
    body.concat(buf, n);
    left -= n;
  }
  return body;
}

String formArg(const String& body, const char* key) {  // "u=ana&p=12%2034" -> URL-decoded value
  const String k = String(key) + "=";
  int start = 0;
  while (start < static_cast<int>(body.length())) {
    int end = body.indexOf('&', start);
    if (end < 0) end = body.length();
    if (body.startsWith(k, start)) {
      String out;
      for (int i = start + k.length(); i < end; i++) {
        char c = body[i];
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && i + 2 < end) {
          const char h[3] = {body[i + 1], body[i + 2], 0};
          c = static_cast<char>(strtol(h, nullptr, 16));
          i += 2;
        }
        out += c;
      }
      return out;
    }
    start = end + 1;
  }
  return "";
}

String jsonResult(bool ok, const char* msg, uint32_t secs) {
  return String("{\"ok\":") + (ok ? "true" : "false") + ",\"s\":" + secs + ",\"msg\":\"" + msg + "\"}";
}

esp_err_t sendJson(httpd_req_t* req, const String& j, const char* status = "200 OK") {
  return respond(req, status, "application/json", j.c_str(), j.length());
}

// ------------------------------------------------------------ handlers -----
esp_err_t handleRoot(httpd_req_t* req) {
  if (!onPortal(req)) return toPanel(req);
  {
    Lock l;
    markActive(clientIp(req), PORTAL_IDLE_KICK_MS);
  }
  return respond(req, "200 OK", "text/html", kPage, strlen(kPage));
}

esp_err_t handleFavicon(httpd_req_t* req) { return respond(req, "404 Not Found", "text/plain", "", 0); }

esp_err_t handleOpen(httpd_req_t* req) {
  if (!onPortal(req)) return respond(req, "404 Not Found", "text/plain", "", 0);
  const uint32_t ip = clientIp(req);
  const String body = readForm(req);
  const std::string user = ical::normalizeUser(formArg(body, "u").c_str());
  String pin = formArg(body, "p");
  pin.trim();

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  markActive(ip, PORTAL_IDLE_KICK_MS);  // trying to sign in counts as using the page
  Strikes& ipS = ipFor(ip);
  Strikes* accS = user.empty() ? nullptr : &accountFor(user);
  if (isBlocked(ipS) || (accS && isBlocked(*accS))) {
    xSemaphoreGive(s_mutex);
    return sendJson(req, jsonResult(false, "Preveč poskusov, poskusite pozneje / Too many attempts, try later", 0), "429 Too Many Requests");
  }
  if (ctl::pinsBlocked()) {
    xSemaphoreGive(s_mutex);
    return sendJson(req, jsonResult(false, "Vrata je zaklenil administrator / Locked by the administrator", 0));
  }

  String kind;
  bool ok = false;
  if (!user.empty() && pin.length() >= 4 && pin.length() <= 8) {
    for (const auto& up : s_users) {
      if (up.first == user.c_str() && ical::safeEquals(up.second.c_str(), pin.c_str())) {
        ok = true;
        kind = "stalni uporabnik";
      }
    }
    ical::Occurrence occ;
    if (!ok && netmgr::timeValid() &&
        schedule::findBooking(ctl::bookingSource(), user.c_str(), pin, static_cast<int64_t>(time(nullptr)),
                              s_beforeS, s_afterS, &occ)) {
      ok = true;
      kind = "rezervacija " + ctl::formatTime(occ.start).substring(11) + "–" + ctl::formatTime(occ.end).substring(11);
    }
  }
  if (ok) {
    ipS = Strikes{};
    if (accS) *accS = Strikes{};
    const uint64_t now = nowMs();
    const bool pause = s_pauseS * 1000ULL > PORTAL_KICK_DELAY_MS;
    portENTER_CRITICAL(&s_mux);
    s_unlockUntilMs = now + s_unlockS * 1000ULL;  // opening again restarts the full time
    if (pause) {  // let "Unlocked" show first, then take the network away
      s_pauseFromMs = now + PORTAL_KICK_DELAY_MS;
      s_pauseUntilMs = now + s_pauseS * 1000ULL;
    }
    portEXIT_CRITICAL(&s_mux);
    if (Phone* ph = phoneByIp(ip)) ph->kickAt = now + PORTAL_KICK_DELAY_MS;
    const uint32_t secs = s_unlockS;
    const uint32_t pauseS = s_pauseS;
    xSemaphoreGive(s_mutex);
    LOGI("portal", "%s unlocked the door (%s)", user.c_str(), kind.c_str());
    if (pause) LOGI("portal", "portal network off for %u s while the door is open", static_cast<unsigned>(pauseS));
    cloud::logEvent("unlock", String(user.c_str()) + " (" + kind + ")");
    return sendJson(req, jsonResult(true, "", secs));
  }
  addStrike(ipS, false);
  if (accS) addStrike(*accS, true);
  xSemaphoreGive(s_mutex);
  LOGW("portal", "failed sign-in for '%s'", user.c_str());
  delay(PORTAL_FAIL_DELAY_MS);  // throttles guessing for everyone
  return sendJson(req, jsonResult(false, "Napačno ime ali PIN / Wrong name or PIN", 0));
}

// Captive portal: every other URL (the phone's "am I online?" checks
// included) is sent to the page. The short body matters for iOS.
esp_err_t handleOther(httpd_req_t* req, httpd_err_code_t) {
  if (!onPortal(req)) return toPanel(req);
  char url[32];
  snprintf(url, sizeof(url), "http://%s/", netmgr::portalIp().toString().c_str());
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", url);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "Sign in to the door", HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void reconfigure() {
  const settings::Data cfg = settings::get();
  std::vector<std::pair<String, String>> users;
  int start = 0;
  while (start < static_cast<int>(cfg.portalUsers.length())) {
    int end = cfg.portalUsers.indexOf('\n', start);
    if (end < 0) end = cfg.portalUsers.length();
    const String line = cfg.portalUsers.substring(start, end);
    start = end + 1;
    const int colon = line.indexOf(':');
    if (colon > 0) users.push_back(std::make_pair(line.substring(0, colon), line.substring(colon + 1)));
  }
  Lock l;
  s_users.swap(users);
  s_enabled = cfg.portalEnabled;
  s_ssid = cfg.portalSsid;
  s_unlockS = cfg.portalUnlockS;
  s_pauseS = cfg.portalPauseS < cfg.portalUnlockS ? cfg.portalPauseS : cfg.portalUnlockS;
  s_beforeS = static_cast<int64_t>(cfg.pinBeforeMin) * 60;
  s_afterS = static_cast<int64_t>(cfg.pinAfterMin) * 60;
  LOGI("portal", "%s, network '%s', %u permanent users, unlock %u s, network off %u s after unlock, PIN window -%u/+%u min",
       s_enabled ? "enabled" : "disabled", s_ssid.c_str(), static_cast<unsigned>(s_users.size()),
       static_cast<unsigned>(s_unlockS), static_cast<unsigned>(s_pauseS), cfg.pinBeforeMin, cfg.pinAfterMin);
}

void begin() {
  s_mutex = xSemaphoreCreateMutex();
  reconfigure();

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.max_open_sockets = 7;    // phones open several connections each
  cfg.lru_purge_enable = true;  // when full, drop the longest-idle connection
  cfg.recv_wait_timeout = 3;
  cfg.send_wait_timeout = 3;
  cfg.stack_size = PORTAL_TASK_STACK;
  cfg.task_priority = 3;       // below the relay control task
  cfg.core_id = APP_CPU;
  if (httpd_start(&s_http, &cfg) != ESP_OK) {
    LOGE("portal", "port-80 web server did not start");
    return;
  }
  static const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = handleRoot, .user_ctx = nullptr},
      {.uri = "/favicon.ico", .method = HTTP_GET, .handler = handleFavicon, .user_ctx = nullptr},
      {.uri = "/open", .method = HTTP_POST, .handler = handleOpen, .user_ctx = nullptr},  // POST only
  };
  for (const httpd_uri_t& r : routes) httpd_register_uri_handler(s_http, &r);
  httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, handleOther);
  httpd_register_err_handler(s_http, HTTPD_405_METHOD_NOT_ALLOWED, handleOther);
}

void tick() {
  if (!netmgr::portalApActive()) return;
  Lock l;
  checkPhones();
}

bool enabled() { return s_enabled; }

bool unlocked() {
  portENTER_CRITICAL(&s_mux);
  const bool on = s_unlockUntilMs && nowMs() < s_unlockUntilMs;
  portEXIT_CRITICAL(&s_mux);
  return on;
}

void lockNow() {
  portENTER_CRITICAL(&s_mux);
  s_unlockUntilMs = 0;
  s_pauseFromMs = s_pauseUntilMs = 0;  // locked again: bring the network back
  portEXIT_CRITICAL(&s_mux);
}

bool apPaused() {
  portENTER_CRITICAL(&s_mux);
  const uint64_t now = nowMs();
  const bool paused = s_pauseUntilMs && now >= s_pauseFromMs && now < s_pauseUntilMs;
  portEXIT_CRITICAL(&s_mux);
  return paused;
}

bool isPortalConnection(const IPAddress& localIp) {
  return netmgr::portalApActive() && localIp == netmgr::portalIp();
}

Info info() {
  Info i;
  i.enabled = s_enabled;
  i.apUp = netmgr::portalApActive();
  portENTER_CRITICAL(&s_mux);
  const uint64_t until = s_unlockUntilMs;
  const uint64_t pauseUntil = s_pauseUntilMs;
  portEXIT_CRITICAL(&s_mux);
  const uint64_t now = nowMs();
  i.unlockLeftS = (until > now) ? static_cast<uint32_t>((until - now + 999) / 1000) : 0;
  i.pausedLeftS = (pauseUntil > now) ? static_cast<uint32_t>((pauseUntil - now + 999) / 1000) : 0;
  Lock l;
  i.ssid = s_ssid;
  i.users = static_cast<int>(s_users.size());
  for (const Phone& p : s_phones) i.phones += p.connected ? 1 : 0;
  return i;
}

}  // namespace portal
