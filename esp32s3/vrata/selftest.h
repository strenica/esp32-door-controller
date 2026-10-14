// Boot-time self-test of the pure logic (timezone rules, iCalendar
// recurrence expansion, PBKDF2). Results go to the serial log; a failure
// does not stop the firmware but is clearly flagged.
#pragma once

namespace selftest {

bool run();

}  // namespace selftest
