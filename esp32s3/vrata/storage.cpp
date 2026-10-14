#include "storage.h"

#include <LittleFS.h>
#include <esp_partition.h>

#include "applog.h"
#include "config.h"

namespace storage {

const char* const kLocalIcs = "/local.ics";
const char* const kRemoteIcs = "/remote.ics";

namespace {
SemaphoreHandle_t s_mutex = nullptr;
bool s_mounted = false;
}  // namespace

bool begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
  // Use whichever data partition the selected Arduino partition scheme has
  // ("spiffs" in the default 4/8 MB schemes, "ffat" in the 16 MB one).
  static const char* const kLabels[] = FS_PARTITION_LABELS;
  for (const char* label : kLabels) {
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label)) continue;
    if (LittleFS.begin(true, "/littlefs", 4, label)) {
      s_mounted = true;
      LOGI("fs", "LittleFS on partition '%s': %u KB used of %u KB", label,
           static_cast<unsigned>(LittleFS.usedBytes() / 1024), static_cast<unsigned>(LittleFS.totalBytes() / 1024));
      return true;
    }
    LOGE("fs", "mounting/formatting partition '%s' failed", label);
  }
  LOGE("fs", "no usable data partition - pick a partition scheme with SPIFFS/FATFS (see README)");
  return false;
}

bool mounted() { return s_mounted; }

size_t freeBytes() {
  if (!s_mounted) return 0;
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}

void wipe() {
  if (!s_mounted) return;
  Lock l;
  LittleFS.remove(kLocalIcs);
  LittleFS.remove(kRemoteIcs);
}

void lock() {
  if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void unlock() {
  if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

}  // namespace storage
