#include "settings.h"

#include <Preferences.h>
#include <esp_random.h>

#include "applog.h"

namespace settings {

namespace {

Preferences s_prefs;
SemaphoreHandle_t s_mutex = nullptr;
Data s_data;

const char* kSsidKeys[] = {"w1s", "w2s", "w3s", "w4s", "w5s"};
const char* kPassKeys[] = {"w1p", "w2p", "w3p", "w4p", "w5p"};
const char* kBakSsidKeys[] = {"b1s", "b2s", "b3s", "b4s", "b5s"};
const char* kBakPassKeys[] = {"b1p", "b2p", "b3p", "b4p", "b5p"};
static_assert(WIFI_MAX_NETWORKS <= 5, "extend the key tables for more networks");

struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mutex); }
};

String newPairCode() {
  static const char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/1/I
  String s;
  for (int i = 0; i < 6; ++i) s += kChars[esp_random() % (sizeof(kChars) - 1)];
  return s;
}

void load() {
  Data& d = s_data;
  d.deviceName = s_prefs.getString("devname", DEFAULT_DEVICE_NAME);
  for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
    d.wifi[i].ssid = s_prefs.getString(kSsidKeys[i], "");
    d.wifi[i].pass = s_prefs.getString(kPassKeys[i], "");
    d.wifiBackup[i].ssid = s_prefs.getString(kBakSsidKeys[i], "");
    d.wifiBackup[i].pass = s_prefs.getString(kBakPassKeys[i], "");
  }
  d.wifiTrial = s_prefs.getBool("wtrial", false);
  d.wifiRev = s_prefs.getULong("wrev", 0);
  d.wifiRejectedRev = s_prefs.getULong("wrej", 0);
  d.setupAp = s_prefs.getUChar("setupap", 0);
  d.serverUrl = s_prefs.getString("srv", DEFAULT_SERVER_URL);
  d.token = s_prefs.getString("tok", "");
  d.pairCode = s_prefs.getString("pair", "");
  d.configRev = s_prefs.getULong("crev", 0);
  d.pollS = s_prefs.getUShort("poll", DEFAULT_POLL_S);
  d.calS = s_prefs.getUShort("cal", DEFAULT_CALENDAR_S);
  d.mode = s_prefs.getUChar("mode", 0);
  d.serverStatus = s_prefs.getChar("sstat", -1);
  d.localUrl = s_prefs.getString("local_url", DEFAULT_LOCAL_CALENDAR_URL);
  d.remoteCalUrl = s_prefs.getString("remote_cal", "");
  d.portalEnabled = s_prefs.getBool("p_en", false);
  d.portalSsid = s_prefs.getString("p_ssid", PORTAL_DEFAULT_SSID);
  d.portalUnlockS = s_prefs.getUShort("p_unl", 600);
  d.portalPauseS = s_prefs.getUShort("p_pause", 540);
  d.pinBeforeMin = s_prefs.getUShort("p_bef", 15);
  d.pinAfterMin = s_prefs.getUShort("p_aft", 15);
  d.portalUsers = s_prefs.getString("p_users", "");
  d.pwRecord = s_prefs.getString("pw", "");
  d.otaTry = s_prefs.getString("ota_try", "");
  d.otaBad = s_prefs.getString("ota_bad", "");
  d.otaSrc = s_prefs.getUChar("ota_src", 0);
}

// Writes only what differs between `o` (old) and `n` (new).
void persist(const Data& o, const Data& n) {
  auto str = [](const char* k, const String& a, const String& b) {
    if (a != b) s_prefs.putString(k, b);
  };
  str("devname", o.deviceName, n.deviceName);
  for (int i = 0; i < WIFI_MAX_NETWORKS; ++i) {
    str(kSsidKeys[i], o.wifi[i].ssid, n.wifi[i].ssid);
    str(kPassKeys[i], o.wifi[i].pass, n.wifi[i].pass);
    str(kBakSsidKeys[i], o.wifiBackup[i].ssid, n.wifiBackup[i].ssid);
    str(kBakPassKeys[i], o.wifiBackup[i].pass, n.wifiBackup[i].pass);
  }
  if (o.wifiTrial != n.wifiTrial) s_prefs.putBool("wtrial", n.wifiTrial);
  if (o.wifiRev != n.wifiRev) s_prefs.putULong("wrev", n.wifiRev);
  if (o.wifiRejectedRev != n.wifiRejectedRev) s_prefs.putULong("wrej", n.wifiRejectedRev);
  if (o.setupAp != n.setupAp) s_prefs.putUChar("setupap", n.setupAp);
  str("srv", o.serverUrl, n.serverUrl);
  str("tok", o.token, n.token);
  str("pair", o.pairCode, n.pairCode);
  if (o.configRev != n.configRev) s_prefs.putULong("crev", n.configRev);
  if (o.pollS != n.pollS) s_prefs.putUShort("poll", n.pollS);
  if (o.calS != n.calS) s_prefs.putUShort("cal", n.calS);
  if (o.mode != n.mode) s_prefs.putUChar("mode", n.mode);
  if (o.serverStatus != n.serverStatus) s_prefs.putChar("sstat", n.serverStatus);
  str("local_url", o.localUrl, n.localUrl);
  str("remote_cal", o.remoteCalUrl, n.remoteCalUrl);
  if (o.portalEnabled != n.portalEnabled) s_prefs.putBool("p_en", n.portalEnabled);
  str("p_ssid", o.portalSsid, n.portalSsid);
  if (o.portalUnlockS != n.portalUnlockS) s_prefs.putUShort("p_unl", n.portalUnlockS);
  if (o.portalPauseS != n.portalPauseS) s_prefs.putUShort("p_pause", n.portalPauseS);
  if (o.pinBeforeMin != n.pinBeforeMin) s_prefs.putUShort("p_bef", n.pinBeforeMin);
  if (o.pinAfterMin != n.pinAfterMin) s_prefs.putUShort("p_aft", n.pinAfterMin);
  str("p_users", o.portalUsers, n.portalUsers);
  str("pw", o.pwRecord, n.pwRecord);
  str("ota_try", o.otaTry, n.otaTry);
  str("ota_bad", o.otaBad, n.otaBad);
  if (o.otaSrc != n.otaSrc) s_prefs.putUChar("ota_src", n.otaSrc);
}

}  // namespace

void begin() {
  s_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_prefs.begin("vrata", false)) LOGE("cfg", "NVS open failed - settings will not persist");

  if (!s_prefs.isKey("init")) {  // first boot (or after a factory reset)
    LOGI("cfg", "first boot: writing defaults");
    const char* ssids[] = {DEFAULT_WIFI1_SSID, DEFAULT_WIFI2_SSID, DEFAULT_WIFI3_SSID};
    const char* passes[] = {DEFAULT_WIFI1_PASSWORD, DEFAULT_WIFI2_PASSWORD, DEFAULT_WIFI3_PASSWORD};
    for (int i = 0; i < WIFI_MAX_NETWORKS && i < 3; ++i) {
      s_prefs.putString(kSsidKeys[i], ssids[i]);
      s_prefs.putString(kPassKeys[i], passes[i]);
    }
    s_prefs.putBool("init", true);
  }

  Lock l;
  load();
  if (s_data.pairCode.isEmpty()) {
    s_data.pairCode = newPairCode();
    s_prefs.putString("pair", s_data.pairCode);
  }
  if (s_data.portalUsers.length() > 3900) s_data.portalUsers = "";  // NVS string limit guard
}

Data get() {
  Lock l;
  return s_data;
}

void modify(const std::function<void(Data&)>& fn) {
  Lock l;
  const Data old = s_data;
  fn(s_data);
  if (s_data.portalUsers.length() > 3900) s_data.portalUsers = old.portalUsers;  // NVS limit ~4000 bytes
  persist(old, s_data);
}

void factoryReset() {
  Lock l;
  s_prefs.clear();
}

}  // namespace settings
