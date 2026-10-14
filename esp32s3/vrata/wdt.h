// Task watchdog helpers (ESP-IDF 5.x API used by Arduino-ESP32 3.x).
// Every long-running task subscribes itself and feeds the watchdog in its
// loop; a hung HTTP transfer or parser loop therefore reboots the board
// instead of freezing the door in whatever state it was in.
#pragma once
#include <esp_task_wdt.h>

namespace wdt {

inline void init(uint32_t timeoutS) {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = timeoutS * 1000;
  cfg.idle_core_mask = 1 << 0;  // keep watching IDLE0 (Wi-Fi core) as the core does
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
}

inline void subscribe() { esp_task_wdt_add(nullptr); }

// Safe to call from any task: only feeds if the caller is subscribed
// (IDF 5 logs an error for esp_task_wdt_reset() from unsubscribed tasks).
inline void feed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

}  // namespace wdt
