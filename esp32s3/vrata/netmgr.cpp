#include "netmgr.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <esp_mac.h>
#include <esp_sntp.h>

#include "applog.h"
#include "cloud.h"
#include "config.h"
#include "portal.h"
#include "wdt.h"

namespace netmgr {

namespace {

enum class ApMode : uint8_t { None, Setup, Portal };

const IPAddress kSetupIp(192, 168, 4, 1);
// Public-style address: Samsung phones say "no internet" instead of "sign in"
// when a portal is on 192.168.x.x (from the gym firmware). It only exists
// inside the door's own network.
const IPAddress kPortalIp(4, 3, 2, 1);

WiFiMulti* s_multi = nullptr;
DNSServer s_dns;
SemaphoreHandle_t s_dnsMutex = nullptr;
SemaphoreHandle_t s_infoMutex = nullptr;

volatile bool s_reconfigure = false;
volatile uint32_t s_setupUntilMs = 0;
volatile ApMode s_ap = ApMode::None;
bool s_haveNetworks = false;
bool s_wasConnected = false;
bool s_servicesStarted = false;
bool s_mdnsStarted = false;
bool s_portalWanted = false;
uint8_t s_setupPolicy = 0;
bool s_trial = false;
uint32_t s_trialStartMs = 0;
uint32_t s_lastConnectedMs = 0;
uint32_t s_connectedSinceMs = 0;
uint32_t s_lastAttemptMs = 0;
String s_hostname;
String s_setupSsid;
String s_portalSsid;
Info s_info;

const char* apName(ApMode m) { return m == ApMode::Setup ? "setup" : (m == ApMode::Portal ? "portal" : ""); }

void setInfo() {
  Info i;
  i.staConnected = WiFi.status() == WL_CONNECTED;
  if (i.staConnected) {
    i.ssid = WiFi.SSID();
    i.ip = WiFi.localIP().toString();
    i.rssi = WiFi.RSSI();
  }
  i.apActive = s_ap != ApMode::None;
  i.apMode = apName(s_ap);
  i.apSsid = s_ap == ApMode::Setup ? s_setupSsid : (s_ap == ApMode::Portal ? s_portalSsid : String());
  if (i.apActive) i.apIp = WiFi.softAPIP().toString();
  i.hostname = s_hostname;
  i.wifiTrial = s_trial;
  xSemaphoreTake(s_infoMutex, portMAX_DELAY);
  s_info = i;
  xSemaphoreGive(s_infoMutex);
}

void loadConfig() {
  const settings::Data cfg = settings::get();
  delete s_multi;
  s_multi = new WiFiMulti();  // no setAllowOpenAP(): that would join ANY open network
  s_haveNetworks = false;
  for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
    if (cfg.wifi[i].ssid.isEmpty()) continue;
    s_multi->addAP(cfg.wifi[i].ssid.c_str(), cfg.wifi[i].pass.isEmpty() ? nullptr : cfg.wifi[i].pass.c_str());
    s_haveNetworks = true;
  }
  s_hostname = hostnameFor(cfg.deviceName);
  s_portalWanted = cfg.portalEnabled;
  s_portalSsid = cfg.portalSsid;
  s_setupPolicy = cfg.setupAp;
  if (cfg.wifiTrial && !s_trial) {
    s_trial = true;  // also after a reboot in the middle of a trial
    s_trialStartMs = millis();
  }
}

void stopAp() {
  if (s_ap == ApMode::None) return;
  xSemaphoreTake(s_dnsMutex, portMAX_DELAY);
  s_dns.stop();
  xSemaphoreGive(s_dnsMutex);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  LOGI("wifi", "%s network stopped", apName(s_ap));
  s_ap = ApMode::None;
}

void startAp(ApMode m) {
  WiFi.mode(WIFI_AP_STA);
  bool ok;
  if (m == ApMode::Setup) {
    ok = WiFi.softAP(s_setupSsid.c_str(), AP_PASSWORD) && WiFi.softAPConfig(kSetupIp, kSetupIp, IPAddress(255, 255, 255, 0));
  } else {
    // Open network; phones use us as DNS (last argument) so every lookup lands here.
    ok = WiFi.softAP(s_portalSsid.c_str(), nullptr, 1, 0, PORTAL_MAX_CLIENTS) &&
         WiFi.softAPConfig(kPortalIp, kPortalIp, IPAddress(255, 255, 255, 0), static_cast<uint32_t>(0), kPortalIp);
  }
  if (!ok) {
    LOGE("wifi", "could not start the %s network", apName(m));
    return;
  }
  xSemaphoreTake(s_dnsMutex, portMAX_DELAY);
  s_dns.start(53, "*", WiFi.softAPIP());
  xSemaphoreGive(s_dnsMutex);
  s_ap = m;
  if (m == ApMode::Setup) {
    LOGW("wifi", "setup network '%s' (password '%s') at http://%s:%d/", s_setupSsid.c_str(), AP_PASSWORD,
         WiFi.softAPIP().toString().c_str(), WEB_PORT);
  } else {
    LOGI("wifi", "portal network '%s' (open) at http://%s/", s_portalSsid.c_str(), WiFi.softAPIP().toString().c_str());
  }
}

ApMode desiredAp(uint32_t now, bool connected) {
  if (s_setupUntilMs && static_cast<int32_t>(s_setupUntilMs - now) > 0) return ApMode::Setup;
  // Portal door: its network is off for a while after an unlock (phones stop reconnecting).
  if (s_portalWanted) return portal::apPaused() ? ApMode::None : ApMode::Portal;
  if (!s_haveNetworks) return ApMode::Setup;
  if (s_setupPolicy == 0 && !connected && now - s_lastConnectedMs > AP_FALLBACK_AFTER_MS) return ApMode::Setup;
  // keep the setup network while a phone uses it, or until Wi-Fi is stable again
  if (s_ap == ApMode::Setup && (WiFi.softAPgetStationNum() > 0 || (connected && now - s_connectedSinceMs < AP_STOP_AFTER_CONNECTED_MS))) {
    return ApMode::Setup;
  }
  return ApMode::None;
}

void startServices() {
  if (!s_servicesStarted) {
    s_servicesStarted = true;
    sntp_set_sync_interval(3600UL * 1000UL);  // runs in the background from now on
    configTzTime(DEVICE_TZ_POSIX, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    LOGI("time", "NTP started (%s, %s, %s)", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  }
  if (!s_mdnsStarted && MDNS.begin(s_hostname.c_str())) {
    MDNS.addService("http", "tcp", WEB_PORT);
    s_mdnsStarted = true;
    LOGI("wifi", "mDNS: http://%s.local:%d/", s_hostname.c_str(), WEB_PORT);
  }
}

void tryConnect() {
  if (!s_haveNetworks) return;
  // WiFiMulti scans and joins the strongest known network (blocks a few s).
  wdt::feed();
  const uint8_t st = s_multi->run(WIFI_CONNECT_TIMEOUT_MS);
  wdt::feed();
  if (st != WL_CONNECTED) LOGW("wifi", "no saved network reachable (status %u)", st);
}

void trialTick(bool connected) {
  if (!s_trial) return;
  if (connected) {
    s_trial = false;
    settings::modify([](settings::Data& d) {
      d.wifiTrial = false;
      for (auto& b : d.wifiBackup) b = settings::WifiNet();
    });
    LOGI("wifi", "new Wi-Fi settings from the server work");
    cloud::logEvent("wifi", "Nove Wi-Fi nastavitve delujejo (" + WiFi.SSID() + ").");
  } else if (millis() - s_trialStartMs > WIFI_TRIAL_MS) {
    s_trial = false;
    settings::modify([](settings::Data& d) {
      for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
        d.wifi[i] = d.wifiBackup[i];
        d.wifiBackup[i] = settings::WifiNet();
      }
      d.wifiRejectedRev = d.wifiRev;
      d.wifiTrial = false;
    });
    LOGE("wifi", "new Wi-Fi settings did not connect - back to the previous networks");
    cloud::logEvent("wifi", "Nove Wi-Fi nastavitve se niso povezale, naprava je spet na prejšnjih omrežjih.");
    s_reconfigure = true;
  }
}

}  // namespace

String hostnameFor(const String& deviceName) {
  String h;
  for (size_t i = 0; i < deviceName.length() && h.length() < 32; ++i) {
    const char c = deviceName[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      h += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    } else if ((c == '-' || c == '_' || c == ' ') && h.length() && h[h.length() - 1] != '-') {
      h += '-';
    }
  }
  while (h.endsWith("-")) h.remove(h.length() - 1);
  // Short names like "V1" become "vrata-v1" so they do not clash on the LAN.
  if (h.length() < 4) h = "vrata-" + h;
  return h;
}

void begin() {
  s_dnsMutex = xSemaphoreCreateMutex();
  s_infoMutex = xSemaphoreCreateMutex();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // works before the Wi-Fi driver starts
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  s_setupSsid = String(AP_SSID_PREFIX) + suffix;

  loadConfig();
  WiFi.persistent(false);  // credentials live in our own NVS namespace
  WiFi.setHostname(s_hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // snappier web pages; the board is mains powered
  WiFi.setAutoReconnect(true);
  s_lastConnectedMs = millis();
  if (s_haveNetworks) {
    LOGI("wifi", "hostname '%s', connecting...", s_hostname.c_str());
  } else {
    LOGW("wifi", "no Wi-Fi configured");
  }
  setInfo();
}

void tick() {
  const uint32_t now = millis();

  if (s_reconfigure) {
    s_reconfigure = false;
    LOGI("wifi", "applying new network settings");
    const String oldHost = s_hostname;
    const ApMode oldAp = s_ap;
    loadConfig();
    stopAp();
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    WiFi.setHostname(s_hostname.c_str());
    WiFi.mode(WIFI_STA);
    if (s_mdnsStarted && oldHost != s_hostname) {
      MDNS.end();
      s_mdnsStarted = false;
    }
    s_wasConnected = false;
    s_lastAttemptMs = 0;
    (void)oldAp;  // the right network of our own is started again below
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!s_wasConnected) {
      s_wasConnected = true;
      s_connectedSinceMs = now;
      LOGI("wifi", "connected to '%s', IP %s, RSSI %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
           WiFi.RSSI());
    }
    s_lastConnectedMs = now;
    startServices();
  } else {
    if (s_wasConnected) {
      s_wasConnected = false;
      LOGW("wifi", "connection lost");
    }
    // Scanning makes phones on our own network drop briefly: retry less often then.
    const uint32_t retry = (s_ap != ApMode::None && WiFi.softAPgetStationNum() > 0) ? WIFI_RETRY_INTERVAL_AP_MS
                                                                                    : WIFI_RETRY_INTERVAL_MS;
    if (s_haveNetworks && (s_lastAttemptMs == 0 || now - s_lastAttemptMs >= retry)) {
      s_lastAttemptMs = now;
      tryConnect();
    }
  }
  trialTick(WiFi.status() == WL_CONNECTED);

  const ApMode want = desiredAp(millis(), WiFi.status() == WL_CONNECTED);
  if (want != s_ap) {
    stopAp();
    if (want != ApMode::None) startAp(want);
  }
  setInfo();
}

void requestReconfigure() { s_reconfigure = true; }

void openSetupAp() {
  const uint32_t until = millis() + SETUP_AP_ON_DEMAND_MS;
  s_setupUntilMs = until ? until : 1;
  LOGI("wifi", "setup network requested for %lu min", SETUP_AP_ON_DEMAND_MS / 60000UL);
}

void applyServerWifi(const settings::WifiNet* nets, int count, uint32_t rev) {
  settings::modify([&](settings::Data& d) {
    if (!d.wifiTrial) {
      for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) d.wifiBackup[i] = d.wifi[i];
    }
    for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) d.wifi[i] = i < count ? nets[i] : settings::WifiNet();
    d.wifiRev = rev;
    d.wifiTrial = true;
  });
  s_trial = true;
  s_trialStartMs = millis();
  LOGW("wifi", "new Wi-Fi settings from the server (revision %lu), trying them", static_cast<unsigned long>(rev));
  s_reconfigure = true;
}

void processDns() {
  if (s_ap == ApMode::None || !s_dnsMutex) return;
  if (xSemaphoreTake(s_dnsMutex, 0) == pdTRUE) {
    if (s_ap != ApMode::None) s_dns.processNextRequest();
    xSemaphoreGive(s_dnsMutex);
  }
}

bool online() { return WiFi.status() == WL_CONNECTED; }

bool timeValid() { return time(nullptr) > 1704067200; }  // after 2024-01-01

bool portalApActive() { return s_ap == ApMode::Portal; }

IPAddress portalIp() { return kPortalIp; }

Info info() {
  xSemaphoreTake(s_infoMutex, portMAX_DELAY);
  Info i = s_info;
  xSemaphoreGive(s_infoMutex);
  return i;
}

}  // namespace netmgr
