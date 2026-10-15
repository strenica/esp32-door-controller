# Vrata

Calendar-driven door access control. Each door runs an ESP32-S3 board that opens
a relay while an event in its Google Calendar is in progress. A small PHP website
manages every door: status, calendars, Wi-Fi, firmware updates, monitoring, logs,
users and roles. The website is in Slovene.

Optional **PIN portal**: a door can run an open Wi-Fi network where phones join
and unlock the door with a name + PIN. PINs come from permanent users or from
calendar bookings titled `Name Surname-123456`.

```
esp32s3/            ESP32-S3 door firmware (Arduino, no third-party libraries)
  vrata/            the sketch (open vrata.ino)
  tools/            gen_assets.sh / .ps1 - regenerates the gzipped CSS header
server/             management website + door API (PHP 8.1+, MySQL/MariaDB)
  oss/              the site itself (drop into a web folder named "oss")
deploy/php/         Dockerfile for the local test server
compose.yaml        local test stack: MariaDB + PHP/Apache
```

---

# Server

The management website for all doors. Each ESP32 door contacts it every
30 s – 60 min (adjustable per door). On each contact the door reports its state
and receives its settings.

**What it does:**
- **Vrata** (doors): status 0–3 per door, "Odkleni zdaj" (unlock now), and adopting new devices.
- **Skupine** (groups): each group has a Google calendar (status 2 = follow the group calendar).
- **Portal (telovadnica / gym):** a door can run an open Wi-Fi network with a PIN page. PINs come from two places:
  - permanent users, managed on the door's page;
  - calendar bookings titled `Ime Priimek-123456`.
- **Nadzor** (monitoring): last contact, door open/locked and why, last calendar download, Wi-Fi signal, firmware, uptime, last reset, memory, errors.
- **Dnevnik** (log): unlocks (who and when), status changes, reboots, changes made on the website or offline on a door.
- **Programska oprema** (firmware): upload a `.bin` and assign it to doors. A door that can't reach the server with the new version goes back to the old one by itself.
- **Uporabniki / Vloge** (users / roles):
  - each role is a set of permissions you tick yourself;
  - each user can be limited to selected groups or doors.

## Requirements
- PHP 8.1 or newer (tested for 8.4) with PDO MySQL, on Apache.
- MySQL 8.0 / MariaDB 10.5+.
- **HTTPS with a valid certificate.** The doors refuse servers whose certificate they can't verify.

## Quick start (local test)

The whole test stack runs in containers; nothing is installed on the host.
Run these from the repository root:

```bash
git clone https://github.com/strenica/esp32-door-controller
cd esp32-door-controller
cp server/oss/config.docker.php server/oss/config.php
docker compose up -d
```

Then open <http://localhost:8000/install.php> and create the first
administrator. Tear down with `docker compose down -v` (the `-v` also drops the
database volume).

To test with a **real door** on the same network, change `base_url` in
`config.php` to this machine's LAN address (e.g. `http://192.168.1.10:8000`) and
set the door's server URL to the same. The doors download their firmware from
`base_url`, so `localhost` will not work for them.

## Installation (phpMyAdmin)
1. **Keep a copy of the old site.** Rename the old `oss` folder (e.g. `oss_old`), or download it first.
2. **Upload the files.** Put the contents of `server/oss/` into the web folder `oss`, so that `https://<your-domain>/oss/login.php` exists.
3. **Create the tables.** In phpMyAdmin, pick the database (or create a new one), then go to **Uvozi** (Import) and choose `install.sql`. All tables start with `vr_`, so they don't clash with older ones.
4. **Configure the database connection.**
   - Copy `config.sample.php` to `config.php`.
   - Fill in the database name, user and password: the ones PHP uses to connect to MySQL.
   - Set `base_url` to your site's address, e.g. `https://doors.example.com/oss` (no trailing slash).
   - Optionally set `site_name` for the footer text.
5. **Create the first administrator.** Open `https://<your-domain>/oss/install.php`. Once one user exists the page does nothing, and you can delete it.
6. **Check the upload limit.** The firmware is about 1.5 MB, and some hosts allow only 2 MB. If the firmware page reports an upload error, raise the limit in the hosting panel or a `.user.ini` file in `oss/`:
   ```
   upload_max_filesize = 8M
   post_max_size = 8M
   ```

**Protecting configuration files:**
- **Apache:** the `.htaccess` files block direct access to `config.php`, `install.sql` and the `lib/` folder.
- **nginx:** `.htaccess` is ignored. Deny `/oss/lib/` and `/oss/install.sql` in the server config. `config.php` is safe either way, because PHP runs it instead of showing it.

## Daily use

### Adding a door
1. Flash the door firmware (see [Firmware](#firmware)) and connect the door to Wi-Fi.
2. Within 30 s it appears on **Vrata** under **Nove naprave čakajo na sprejem** (new devices waiting), with a **pairing code** (koda za povezavo).
3. Compare the code with the code on the door's own page (`http://<door IP>:5000`, Status table) or in its serial log. Accept the door only if the codes match.
4. Enter its name (V1 …) and group, and press **Sprejmi** (accept). The door collects its key at the next contact and from then on follows the server.
5. **To replace a broken door's hardware:** choose the old door under *ali zamenjava za* (or replace). The new board takes over all of its settings.

### Status
| Status | Door |
|---|---|
| 0 Zaklenjeno (LOCK) | locked; **PINs don't work either** |
| 1 Odprto (OPEN) | open |
| 2 Urnik skupine (SCHEDULE REMOTE) | open during events of the **group** calendar |
| 3 Lokalni urnik (SCHEDULE LOCAL) | open during events of the door's **own** calendar (door page → Koledar) |

**Odkleni zdaj za X min** (unlock now for X minutes) opens a door in any status, including LOCK. The door picks it up at its next contact.

### Calendars
Use the **secret address in iCal format** (Google Calendar → settings of the calendar → *Integrate calendar*). Keep these calendars private: anyone who can see them also sees the booking PINs.

### PIN portal (gym door)
On the door's page, under **Portal**, you can:
- switch the portal on;
- name its network;
- set how long a door stays unlocked;
- set how many minutes **before** and **after** a booking the PIN works (0–240, default 15 / 15).

There are two kinds of PIN:
- **Bookings:** an event titled `James Alice-123456` (or `James Alice - 123456`) lets user **`jamesalice`** in with PIN **123456** during the booking window.
  - The name becomes the username: lowercase, spaces and hyphens removed, and č/š/ž/ć/đ and accented letters simplified (`Ana Šolar` → `anasolar`).
  - People can type either their full name or the username on the phone.
  - The PIN has 4–8 digits.
  - A booking event does **not** open the door by itself. Events without `-PIN` keep the door open as usual.
- **Permanent users** (door page → *Stalni uporabniki portala*): the PIN works at any time except during LOCK.

How it behaves:
- **One correct PIN** opens the door for the set time, and the phone is then disconnected from the door's network.
- **The door's network switches off after an unlock** for the time set in *Omrežje po odklepu izklopi za (s)* (switch the network off for … seconds after an unlock). Phones and tablets that remember the network then stop trying to reconnect while the door is open anyway.
  - The default is the unlock time − 60 s, so the network is back one minute before the door locks and the next person can extend it.
  - 0 keeps the network on.
  - LOCK brings the network back immediately.
- **Wrong guesses:** after 5 wrong tries, that phone waits 1 minute. The account waits 1, 2, 4, then 8 minutes.
- **Unlock log:** every unlock appears in **Dnevnik** (log) with the username.
- **If the sign-in page doesn't open by itself:** the door is always at `4.3.2.1`, so the user can type that into a browser. Put this where it is visible *before* connecting — a sticker on the door, or at the end of the network name (e.g. `Vrata-4.3.2.1`, set under *Portal → ime omrežja*). It cannot go on the sign-in page itself: anyone reading it is already there.

### Roles and users
- **Vloge** (roles): tick what each role may do: view doors, change status, calendars, gym users, portal settings, intervals, monitoring, log, device name, Wi-Fi, firmware, users. *Administrator* always has everything.
- **Uporabniki** (users): each user has one role and sees either all doors, or only selected groups and/or doors.
- **Example:** a kindergarten employee with role *Osebje* and only the VRTEC group sees and changes only the kindergarten doors.

### Firmware updates
1. **Raise `FW_VERSION` in `vrata/config.h` first.** A door skips any version it is already running, so uploading a build with the same version number does nothing at all.
2. Build it: *Sketch → Export Compiled Binary* in the Arduino IDE, or `arduino-cli compile --export-binaries esp32s3/vrata` (writes `esp32s3/vrata/build/esp32.esp32.esp32s3/vrata.ino.bin`).
3. Upload `vrata.ino.bin` — **not** `.merged.bin` — on **Programska oprema** (firmware). The version is read from the file.
4. Assign it to the doors you want. Each door installs it at its next contact, restarts, and must reach the server again within 10 minutes. Otherwise it goes back to the previous version and the monitoring page shows the version as rejected.

### Wi-Fi from the server
On the door's page (*Wi-Fi naprave*), tick "Wi-Fi omrežja nastavlja strežnik" (Wi-Fi networks are set by the server) and enter up to 3 networks. If the door can't connect with the new networks within 5 minutes, it returns to its previous ones and reports this in monitoring. Without the tick, the door keeps its own networks.

Gym doors keep the setup network off. A short press on the door's BOOT button opens it for 10 minutes.

## Door API reference
`POST /oss/api/door.php` with a JSON body:
```json
{"mac":"AA:BB:CC:DD:EE:FF","token":"<door key>","pair_code":"K7Q2MX","fw":"2.0.0",
 "boot_id":"1a2b3c4d","reset_reason":"vklop napajanja","local_ip":"10.0.0.21",
 "report":{...state...},"log":[{"t":1730000000,"type":"unlock","msg":"jamesalice (rezervacija 18:00–19:00)"}]}
```
| Reply | Meaning |
|---|---|
| `{"ok":true,"state":"active","config":{...}}` | normal reply: the door's settings |
| `{"state":"pending"}` | not adopted yet |
| `{"state":"adopted","token":"…"}` | first reply after adoption, carries the door's key |
| HTTP 401 | the key is no longer valid (door deleted), so the door registers again as new |

- **Door keys:** stored only as SHA-256 hashes.
- **Firmware downloads:** `api/firmware.php` serves a binary only to the door it's assigned to.

## Upgrading the website (Nadgradnja)
Upload the new files over the old ones. Keep your `config.php`, and delete `install.php` again if you had removed it. New database columns are added automatically the first time any page or door uses the database.

If the database user isn't allowed to change tables, run this once in phpMyAdmin → **SQL** instead:
```sql
-- version 2.1 (portal network pause)
ALTER TABLE vr_doors ADD COLUMN portal_pause_s INT NOT NULL DEFAULT 540 AFTER portal_unlock_s;
```

## Backups
Back up the database with phpMyAdmin → **Izvoz** (Export). Everything (doors, users, PINs, firmware, log) lives in the `vr_*` tables.

---

# Firmware

ESP32-S3 firmware for one door. Each door opens its relay while an event in its
calendar is in progress.

A local page on **port 5000** shows the door's state. You can change settings
there only while the server is unreachable, so the door stays usable when the
internet is down.

The firmware uses no third-party libraries, only what ships with the ESP32
Arduino core.

## Parts per door

| Part | Notes |
|---|---|
| ESP32-S3 development board, **8 MB flash recommended** | e.g. ESP32-S3-DevKitC-1 N8R2, N8R8 or N16R8. PSRAM is not needed. |
| 5 V USB power supply, at least 1 A | Powers the board and the relay board |
| USB cable that carries data | Only needed for the first flash |
| 1-channel relay board, enclosure and 3-wire cable | Must switch reliably from 3.3 V logic — see below |
| 10 kΩ resistor (recommended) | GPIO4 → GND, keeps the door locked while the board starts |

## Wiring

| ESP32-S3 | Relay board |
|---|---|
| 5V | VCC |
| GND | GND |
| **GPIO4** | IN |
| BOOT (GPIO0) | – (on-board button: short press = setup network for 10 min, hold 10 s = local password reset, hold 30 s = factory reset) |

- **Signal level.** GPIO4 is driven HIGH (3.3 V) to open the door (`RELAY_ACTIVE_HIGH 1`), so the relay board must switch from 3.3 V logic.
- **Pins.** All pins are defined in `vrata/config.h`, which also lists the pins that were avoided and why.
- **Gym door:** the relay only controls the electric lock. Make sure the door can always be opened **from inside** by its handle or an exit button.

## Build and flash

Install *esp32 by Espressif Systems* 3.x (tested with 3.3.12) in the Arduino IDE,
open `vrata/vrata.ino`, and choose in **Tools**:

| Setting | 8 MB board (recommended) | 16 MB board | 4 MB board |
|---|---|---|---|
| Board | ESP32S3 Dev Module | ESP32S3 Dev Module | ESP32S3 Dev Module |
| Flash Size | 8MB | 16MB | 4MB |
| Partition Scheme | **8M with spiffs (3MB APP/1.5MB SPIFFS)** | 16M Flash (3MB APP/9.9MB FATFS) | Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS) |
| USB CDC On Boot | Disabled | same | same |

The firmware is about 1.45 MB, so the 4 MB *Default* scheme is too small. Updates from the server need a scheme with two app slots; every scheme above has them.

With those settings in place, the IDE's **Upload** button does the same as the Arduino CLI commands below.

### Which USB socket to use

A DevKitC-1 has **two USB-C sockets** and they are not interchangeable:

| Socket | Windows | Linux | Arduino `Serial` log |
|---|---|---|---|
| **UART** (CP2102N bridge) | `COMx`, "Silicon Labs CP210x" | `/dev/ttyUSB0` | **yes** |
| **USB** (native USB-JTAG, GPIO19/20) | `COMx`, "USB Serial Device" | `/dev/ttyACM0` | only if *USB CDC On Boot* is **Enabled** |

The settings above use *USB CDC On Boot: Disabled*, so the firmware's log goes to
UART0 — that is, **the log only shows up on the UART socket**. Flashing works over
either socket.

Either plug into the **UART** socket, or set *USB CDC On Boot* to **Enabled** and
keep using the USB socket (then the log appears there too).

### Wiping the flash first

An upload replaces the firmware but **leaves NVS alone** — stored settings,
Wi-Fi credentials, the server key and the pairing code all survive. A board that
was configured before comes back with its old state, which looks like the new
firmware was ignored. For a new door, erase first.

**Call the bundled esptool by its full path.** Most Linux machines have two and a
bare `esptool` picks the wrong one:

| Which | Version | Erase command | Works? |
|---|---|---|---|
| Bundled with the ESP32 core | 5.x | `erase-flash` | yes |
| Debian/Ubuntu `esptool` package | 4.x | `erase_flash` | no |

The Debian package rejects `erase-flash` (`invalid choice`) *and* ships without
the ESP32-S3 stub flasher, so `erase_flash` there fails too with
`FileNotFoundError: ...stub_flasher_32s3.json`. Use the full path:

```bash
# Linux
~/.arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool --port /dev/ttyUSB0 erase-flash

# Windows — same idea, adjust the version folder if yours differs
%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe --port COM5 erase-flash
```

Erasing removes the firmware too — **flash straight after**, or the board has
nothing to boot. If you'd rather not, flash first and then hold **BOOT for 30 s**
on the running firmware: that clears NVS and the calendars and keeps the app.

### Flashing on Windows

1. **Driver.** The UART socket is a Silicon Labs CP2102N bridge; Windows normally
   installs it on its own. If no port ever appears, install the *CP210x VCP driver*
   from Silicon Labs and replug the board. Some clone boards use a CH340 chip
   instead — those need the CH340 driver.
2. **Find the port.** Any of these works:
   - **Device Manager** → *Ports (COM & LPT)* → e.g. "Silicon Labs CP210x USB to UART Bridge (COM5)".
   - **Arduino IDE** → *Tools → Port* → pick the entry that appears when you plug the board in.
   - **Arduino CLI** → `arduino-cli board list` lists the port, board name and FQBN.
3. **Compile and upload** from the repository root (the settings come from `sketch.yaml`, 8 MB):
   ```
   arduino-cli compile --upload -p COM5 esp32s3/vrata
   ```
4. **Watch the log** (`Ctrl+C` to stop):
   ```
   arduino-cli monitor -p COM5 -c baudrate=115200
   ```

### Flashing on Linux

1. **Permission** (once, then log out and back in):
   ```bash
   sudo usermod -aG dialout $USER
   ```
2. **Find the port.** After plugging the board in:
   ```bash
   arduino-cli board list          # port, board name and FQBN
   ls /dev/ttyUSB* /dev/ttyACM*    # ttyUSB0 = UART socket, ttyACM0 = USB socket
   ```
   `dmesg | tail` right after plugging it in also names the new port.
3. **Compile and upload** from the repository root:
   ```bash
   arduino-cli compile --upload -p /dev/ttyUSB0 esp32s3/vrata
   ```
4. **Watch the log** (`Ctrl+C` to stop):
   ```bash
   arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
   ```

### If the upload fails

- **No port at all** → wrong driver (Windows), missing `dialout` membership (Linux), or a charge-only USB cable.
- **"Failed to connect"** → put the board in download mode by hand: hold **BOOT**, tap **RESET**, release **BOOT**, then upload again. The DevKitC-1 usually resets itself and this is not needed.
- **Port busy** → close the serial monitor (or the Arduino IDE Serial Monitor) before uploading; an open port blocks the upload.

## Before flashing (`vrata/config.h` + `vrata/secrets.h`)
Your private values go in **`secrets.h`**, which is not committed. Copy the template first:

```bash
cp esp32s3/vrata/secrets.sample.h esp32s3/vrata/secrets.h
```

- `DEFAULT_SERVER_URL` (`secrets.h`): address of your management server, no trailing slash, with a valid HTTPS certificate.
- `DEFAULT_ADMIN_PASSWORD` (`secrets.h`): password of the door's **local** page (user `admin`). Change it from `change-me-please`.
- `DEFAULT_WIFI1_SSID/PASSWORD` … `3` (`secrets.h`): optional. You can also enter them later through the setup network, or set them on the server.
- `AP_PASSWORD` (`secrets.h`): password of the setup network (`VRATA-SETUP-xxxx`). Set your own — the `config.h` default (`vrata-setup`) is public.
- `FW_VERSION` (`config.h`): raise it for every new release, e.g. `2.0.1`. The server reads it from the uploaded file, and a door installs only a version it isn't already running.

`secrets.h` is optional: without it the generic defaults in `config.h` apply and the sketch still compiles.

### Default credentials

What you need to reach a freshly flashed board, if you flash **without** a `secrets.h`:

| Where | Login |
|---|---|
| Setup network `VRATA-SETUP-xxxx` | password **`vrata-setup`** |
| Local page `http://192.168.4.1:5000/` | user **`admin`**, password **`change-me-please`** |
| Server URL it will try | `https://example.com/oss` — a placeholder, so it will not reach any real server |

Copying `secrets.sample.h` to `secrets.h` changes the setup-network password to **`change-me-too`** (the sample sets `AP_PASSWORD`). Whatever your `secrets.h` says wins — check it there if the default does not work.

## Putting a door into service
1. **Erase the flash, then flash the board** (see [Wiping the flash first](#wiping-the-flash-first)) — on a used board the old Wi-Fi and server key survive a normal upload. Power it up. With no Wi-Fi settings it opens `VRATA-SETUP-xxxx` (default password `vrata-setup`). Connect a phone, open `http://192.168.4.1:5000/` and log in as `admin` (default password `change-me-please`). In *Settings*, enter the site Wi-Fi. Both defaults are listed under [Default credentials](#default-credentials).
2. **The door appears on the server.** It contacts the server and shows up on the site's **Vrata → Nove naprave** (new devices) with a **pairing code**. The same code is shown on the door's page under *Status → Server* and in the serial log.
3. **Adopt it.** If the codes match, press **Sprejmi** (accept) and give the door its name (V1 …) and group. The door collects its key within 30 s, then follows the server: status, calendar, portal, intervals.
4. **The local page becomes read-only.** From now on it shows the door's state only. If the server can't be reached for 3 check intervals (at least 3 minutes), the page becomes editable.
   - You can then use Start/Stop/schedule, the local calendar, the name and Wi-Fi.
   - These changes go into the server log as "spremembe brez povezave" (changes made offline).
   - When the server is back, its settings apply again.

## How a door behaves

| Server status | Door | PIN portal |
|---|---|---|
| 0 LOCK | locked | **no PIN works** |
| 1 OPEN | open | – |
| 2 SCHEDULE REMOTE | open during events of the group calendar | PINs work |
| 3 SCHEDULE LOCAL | open during events of its own calendar | PINs work |
| "Odkleni zdaj" (unlock now) on the server | open until the set time, **in every status** | – |

**Timing**
- **Server contact:** every *poll interval* (default 60 s, set per door on the server, 30 s – 60 min).
- **Calendar download:** every *calendar interval* (default 5 min, 1 – 60 min).
- **Relay:** checked every 250 ms. A door opens within about 1 s of an event's start.

**Bookings and permanent users (PIN portal enabled on the server)**
- **Bookings** are events titled `Ime Priimek-123456`, with a PIN of 4–8 digits.
  - They do **not** open the door. They let that person open it with the PIN, from *x* min before the start until *y* min after the end (default 15/15, set on the server).
  - The username is the name in lowercase without spaces or diacritics: `Ana Šolar` → `anasolar`. Typing the full name on the phone also works.
- **Permanent users** come from the server and can open at any time except during LOCK.
- **Opening:** a correct PIN opens the door for the set time (default 10 min), and the phone is then disconnected from the door's network.
- **Network pause:** after an unlock the door's network switches off, so devices stop reconnecting. By default this lasts the unlock time − 60 s, which brings the network back one minute before the door locks. 0 keeps the network on; set it per door on the server.
- **Wrong guesses:** after 5 wrong tries the phone waits 60 s, and the account waits 1, 2, 4, then 8 minutes.
- **Offline:** bookings and permanent users are cached on the door and keep working without internet.
- **One network of its own:** the door can run only one network, so on a gym door the setup network appears only after a **short BOOT press** (for 10 minutes; the portal pauses meanwhile).
- **Port 5000 is blocked from the portal network.** Phones on the open network can only reach the PIN page.

**Firmware updates from the server**
1. Raise `FW_VERSION`, then use *Sketch → Export Compiled Binary*. Upload `vrata.ino.bin` on the site's **Programska oprema** (firmware) page and assign it to doors.
2. Each door downloads it at its next contact (checking size and MD5), installs it into the spare slot and restarts.
3. **The new firmware must reach the server within 10 minutes.** Otherwise the board automatically returns to the old firmware, and the server shows the version as rejected.
4. A crash before that point also triggers the automatic rollback.

**Wi-Fi from the server**
- **Trial:** when the admin sets Wi-Fi on the server, the door tries the new networks. If none connects within 5 minutes, it goes back to the previous ones and reports this.
- **Without server-managed Wi-Fi:** the door keeps its own networks.

**Recovery**
| Action | Result |
|---|---|
| BOOT short press | setup network for 10 min |
| BOOT hold 10 s | local password back to `DEFAULT_ADMIN_PASSWORD` |
| BOOT hold 30 s | factory reset: settings, Wi-Fi, server key and calendars erased. The door then appears on the server as new (adopt it as a *replacement* for its old entry to keep its settings) |
| Serial 115200 / `http://<ip>:5000/log` | log; `/api/status` gives JSON |

## Behaviour notes and limits

- **Events start on time** — within about 1 s of the event's start.
- **The server holds the settings, the monitoring and the log.** Each door has its own key, and the calendar links are never published.
- **The local page is read-only while the server is reachable.** It becomes editable once the server has been unreachable for three poll intervals.
- **Manual ON is not restored after a reboot or power cut** — a door should not come back open by itself. If the server still says OPEN, the first contact opens the door again.
- **Every calendar download is validated** (`BEGIN:` / `END:VCALENDAR`) before it replaces the stored one.
- **Local and group calendars are stored separately**, so they can never overwrite each other.
- **All-day events are ignored** (`ALLDAY_EVENTS_ACTIVATE`).
- **Passwords** use PBKDF2-HMAC-SHA256. Logins are throttled and forms are CSRF-protected.
- **Timezones:** a built-in TZID table plus the calendar's own VTIMEZONE blocks. Unknown zones are skipped.
- **Recurrence rules:** BYWEEKNO, BYYEARDAY, BYHOUR/BYMINUTE and hourly repeats aren't supported; such an event uses only its first date. Google's editor doesn't produce these.
- **Limits:** the calendar file can be at most 1 MB, with up to 400 occurrences in the 12-day window.
- **No battery clock:** after a power cut the schedule waits for NTP (193.2.4.6 first). Until then the door stays locked; OPEN and unlock still work.
- **The PIN portal is configured entirely on the server** — permanent users, PINs, the network name, the unlock time. The open network carries no admin page and no serial commands.

## Test checklist (real hardware)

**Boot and hardware**
- [ ] The log shows `self-test passed`, which covers timezones, recurrences, bookings and PBKDF2. Report any FAIL line.
- [ ] The relay doesn't click at power-on or while flashing.

**Server link**
- [ ] The door appears on the site as new, with the same pairing code as on its page. After **Sprejmi** (accept), the server state becomes "connected" within 30 s.
- [ ] The local page shows the blue "managed by the server" box, and its buttons are disabled.
- [ ] Status 0/1/2/3 on the server: the relay follows within one poll interval.
- [ ] "Odkleni zdaj 1 min" (unlock now for 1 minute): the door opens even in LOCK and closes after 1 minute.
- [ ] Set the poll interval to 30 s on the server: monitoring shows contacts about every 30 s.
- [ ] Disconnect the internet for more than 3 minutes. The page becomes editable and Start works. When the internet is back, the server's status applies again, and the log shows the offline change.

**Calendar**
- [ ] With status 3 and a test event 2 minutes ahead, the relay opens within about 1 s of the start and closes at the end.
- [ ] A deleted instance of a recurring event doesn't open the door.

**PIN portal (gym)**
- [ ] Switch the portal on for the door. The network appears, and a phone shows the sign-in page.
- [ ] Add permanent user "Test Oseba" with PIN 123456. Typing `Test Oseba` / `123456` opens the door; the phone is disconnected about 4 s later.
- [ ] Add booking `Test Oseba-4321` in the calendar, 10 min from now. PIN 4321 works from 5 min before the start, and doesn't work 20 min before.
- [ ] Status LOCK: the page says the door is locked by the administrator.
- [ ] 5 wrong PINs: "Too many attempts".
- [ ] From the portal network, `http://4.3.2.1:5000/` is refused.
- [ ] The server log (Dnevnik) shows every unlock with the username.

**Updates and recovery**
- [ ] Upload `vrata.ino.bin` with a higher `FW_VERSION`. The door installs it, restarts, and the server shows the new version.
- [ ] A Wi-Fi setting with a wrong password from the server: after 5 minutes the door is back on its old network and reports the rejection.
- [ ] A short BOOT press on a gym door: the setup network appears for 10 minutes.
- [ ] Leave the door running 24 h: free memory in monitoring stays roughly constant, with no watchdog resets.

## Firmware source layout
```
vrata/              the Arduino sketch (open vrata.ino)
  config.h          all pins, defaults, intervals, wiring table
  secrets.sample.h  template for secrets.h (private server URL, password, Wi-Fi)
  cloud.*           server link, door key, firmware updates + rollback
  portal.*          PIN portal (port 80), phone handling, captive redirects
  controller.*      modes, relay decision, calendar jobs, BOOT button, tasks
  webui.*           local page (port 5000), local firmware upload
  netmgr.*          Wi-Fi (3 networks, server trial), setup/portal network, NTP, mDNS
  ical.* civil.h    iCalendar parser, recurrences, bookings (pure C++)
  tzconv.*          timezone rules (pure C++)
  schedule.* fetch.* settings.* storage.* auth.* relay.* applog.* wdt.h selftest.*
  src/generated/    bootstrap_css.h - gzipped CSS for the local page
```

**FreeRTOS tasks**

| Task | Priority | Job |
|---|---|---|
| control | 4 | relay every 250 ms, BOOT button, portal phones |
| net | 2 | Wi-Fi, NTP, server, calendars, updates |
| web | 2 | page on port 5000 |
| httpd | 3 | port 80: portal / redirect |

control, net and web are watched by the task watchdog (60 s).

### Regenerating the local page CSS
The local page serves a gzipped copy of Bootstrap 4.5.2 as a C header. If the CSS
is ever replaced, regenerate the header from the **one** copy of the CSS at
`server/oss/assets/bootstrap.min.css`:

```bash
esp32s3/tools/gen_assets.sh              # POSIX shell
```
```powershell
powershell -ExecutionPolicy Bypass -File esp32s3\tools\gen_assets.ps1
```

The header is committed, so this is only needed when the CSS changes.

---

# Configuration (not committed)

Two files hold private values and are listed in `.gitignore`:

| File | From | Holds |
|---|---|---|
| `server/oss/config.php` | `config.sample.php` / `config.docker.php` | database credentials, `base_url`, `site_name` |
| `esp32s3/vrata/secrets.h` | `secrets.sample.h` | server URL, local admin password, Wi-Fi networks |

Both have committed templates, so a fresh checkout builds without them.

# Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
