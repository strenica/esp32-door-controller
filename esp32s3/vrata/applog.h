// Serial logging with a RAM ring buffer (shown at /log in the web UI).
// Replaces the print() output of the Python scripts, which went to
// /dev/null via cron; here it goes to the USB serial port at 115200 baud.
#pragma once
#include <Arduino.h>

namespace applog {

void begin();
void write(char level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
String dump();  // oldest -> newest

}  // namespace applog

#define LOGE(tag, ...) applog::write('E', tag, __VA_ARGS__)
#define LOGW(tag, ...) applog::write('W', tag, __VA_ARGS__)
#define LOGI(tag, ...) applog::write('I', tag, __VA_ARGS__)
