// Door relay output (was: pinctrl set 23 op pn dh / pinctrl set 23 dl).
// Only the control task changes the relay after boot.
#pragma once

namespace relay {

void begin();         // drive the inactive level as early as possible
void set(bool on);    // on = door open
bool get();

}  // namespace relay
