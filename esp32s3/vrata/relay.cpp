#include "relay.h"

#include <Arduino.h>

#include "config.h"

namespace relay {

namespace {
volatile bool s_on = false;

inline uint8_t level(bool on, bool activeHigh) { return (on == activeHigh) ? HIGH : LOW; }
}  // namespace

void begin() {
  // Set the output latch before enabling the driver so the pin never
  // glitches to the active level.
  digitalWrite(PIN_RELAY, level(false, RELAY_ACTIVE_HIGH));
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, level(false, RELAY_ACTIVE_HIGH));
#if PIN_STATUS_LED >= 0
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, level(false, STATUS_LED_ACTIVE_HIGH));
#endif
  s_on = false;
}

void set(bool on) {
  // Re-written every cycle like ical.py re-ran pinctrl every 5 s; cheap and
  // self-healing if something else ever touched the pin.
  digitalWrite(PIN_RELAY, level(on, RELAY_ACTIVE_HIGH));
#if PIN_STATUS_LED >= 0
  digitalWrite(PIN_STATUS_LED, level(on, STATUS_LED_ACTIVE_HIGH));
#endif
  s_on = on;
}

bool get() { return s_on; }

}  // namespace relay
