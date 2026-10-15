// Serial logging with a RAM ring buffer (shown at /log in the web UI).
// Log lines go to the serial port at 115200 baud, and the most recent ones
// stay in the ring buffer for the web pages.
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
