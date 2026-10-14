// Local control panel on port 5000 (replaces the Flask app in rpi5/web-gui).
//
// Same look, buttons and messages as app.py. While the door is managed by the
// server the page is read-only (status only); it becomes editable when the
// server has been unreachable for a few polls. Additions: status table,
// server URL + pairing code, device name / Wi-Fi, /log, /api/status and a
// firmware upload page (/update). Port 80 is handled by the portal module.
// The panel is refused to clients of the open PIN-portal network.
#pragma once

namespace webui {

void begin();  // starts the web task

}  // namespace webui
