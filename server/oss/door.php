<?php
require __DIR__ . '/lib/web.php';
$me = require_login();
if (!can('doors.view')) deny();

$door = load_door((int)($_GET['id'] ?? $_POST['id'] ?? 0));
$id = (int)$door['id'];
$self = 'door.php?id=' . $id;

function changed(string $what): void
{
    global $id, $me, $door;
    bump_rev([$id]);
    log_event($id, (int)$me['id'], 'config', $what . ' (' . $me['username'] . ')');
    flash('success', $what . '. Vrata spremembo dobijo ob naslednjem preverjanju.');
}

function valid_url_or_empty(string $u): bool
{
    return $u === '' || (bool)preg_match('#^https?://\S+$#i', $u);
}

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $section = (string)($_POST['section'] ?? '');
    switch ($section) {
        case 'basic':
            if (!can('device.edit')) deny();
            $name = trim((string)($_POST['name'] ?? ''));
            $groupId = (int)($_POST['group_id'] ?? 0) ?: null;
            if (!preg_match('/^[A-Za-z0-9_-]{1,32}$/', $name)) {
                flash('danger', 'Ime: 1–32 znakov (črke, številke, - in _).');
            } elseif (q('SELECT id FROM vr_doors WHERE name = ? AND id <> ?', [$name, $id])->fetch()) {
                flash('danger', 'To ime že uporabljajo druga vrata.');
            } else {
                q('UPDATE vr_doors SET name = ?, description = ?, group_id = ? WHERE id = ?',
                  [$name, mb_substr(trim((string)($_POST['description'] ?? '')), 0, 255), $groupId, $id]);
                changed('Osnovni podatki shranjeni');
            }
            break;

        case 'status':
            if (!can('status.edit')) deny();
            $status = (int)($_POST['status'] ?? -1);
            if (isset(STATUS_LABELS[$status])) {
                q('UPDATE vr_doors SET status = ? WHERE id = ?', [$status, $id]);
                changed('Status: ' . STATUS_LABELS[$status]);
            }
            break;

        case 'open':
            if (!can('status.edit')) deny();
            $min = clamp_int($_POST['minutes'] ?? 10, 1, 720);
            q('UPDATE vr_doors SET open_until = ? WHERE id = ?', [gmdate('Y-m-d H:i:s', time() + $min * 60), $id]);
            changed("Takojšnji odklep za $min min");
            break;

        case 'open_cancel':
            if (!can('status.edit')) deny();
            q('UPDATE vr_doors SET open_until = NULL WHERE id = ?', [$id]);
            changed('Takojšnji odklep preklican');
            break;

        case 'calendar':
            if (!can('calendar.edit')) deny();
            $url = trim((string)($_POST['local_ical_url'] ?? ''));
            if (!valid_url_or_empty($url)) {
                flash('danger', 'Naslov koledarja mora začeti s https://');
            } else {
                q('UPDATE vr_doors SET local_ical_url = ? WHERE id = ?', [$url, $id]);
                changed('Lokalni koledar shranjen');
            }
            break;

        case 'timing':
            if (!can('timing.edit')) deny();
            q('UPDATE vr_doors SET poll_s = ?, calendar_s = ? WHERE id = ?',
              [clamp_int($_POST['poll_s'] ?? 60, POLL_MIN, POLL_MAX), clamp_int($_POST['calendar_s'] ?? 300, CAL_MIN, CAL_MAX), $id]);
            changed('Intervali shranjeni');
            break;

        case 'portal':
            if (!can('portal.config')) deny();
            $ssid = trim((string)($_POST['portal_ssid'] ?? ''));
            if ($ssid === '' || strlen($ssid) > 32) {
                flash('danger', 'Ime omrežja: 1–32 znakov.');
                break;
            }
            $unlock = clamp_int($_POST['portal_unlock_s'] ?? 600, 10, 3600);
            // Omrežje je lahko izklopljeno največ toliko časa, kot so vrata odklenjena.
            $pause = min(clamp_int($_POST['portal_pause_s'] ?? 0, 0, 3600), $unlock);
            q('UPDATE vr_doors SET portal_enabled = ?, portal_ssid = ?, portal_unlock_s = ?, portal_pause_s = ?,
               pin_before_min = ?, pin_after_min = ? WHERE id = ?', [
                !empty($_POST['portal_enabled']) ? 1 : 0, $ssid, $unlock, $pause,
                clamp_int($_POST['pin_before_min'] ?? 15, 0, 240), clamp_int($_POST['pin_after_min'] ?? 15, 0, 240), $id]);
            changed('Nastavitve portala shranjene');
            break;

        case 'portal_add':
            if (!can('portal.users')) deny();
            $display = trim((string)($_POST['display_name'] ?? ''));
            $username = normalize_username($display);
            $pin = trim((string)($_POST['pin'] ?? ''));
            if ($pin === '') $pin = random_pin();
            if ($username === '') {
                flash('danger', 'Vpišite ime (iz njega nastane uporabniško ime).');
            } elseif (!valid_pin($pin)) {
                flash('danger', 'PIN: 4–8 številk.');
            } else {
                q('INSERT INTO vr_portal_users (door_id, username, display_name, pin, active, created_at) VALUES (?, ?, ?, ?, 1, ?)
                   ON DUPLICATE KEY UPDATE display_name = VALUES(display_name), pin = VALUES(pin), active = 1',
                  [$id, $username, mb_substr($display, 0, 100), $pin, now_utc()]);
                bump_rev([$id]);
                log_event($id, (int)$me['id'], 'portal', "Stalni uporabnik $username dodan/posodobljen ({$me['username']}).");
                flash('success', "Uporabnik $username shranjen. PIN: $pin");
            }
            break;

        case 'portal_del':
            if (!can('portal.users')) deny();
            $u = q('SELECT username FROM vr_portal_users WHERE id = ? AND door_id = ?', [(int)($_POST['user_id'] ?? 0), $id])->fetch();
            if ($u) {
                q('DELETE FROM vr_portal_users WHERE id = ? AND door_id = ?', [(int)$_POST['user_id'], $id]);
                bump_rev([$id]);
                log_event($id, (int)$me['id'], 'portal', "Stalni uporabnik {$u['username']} odstranjen ({$me['username']}).");
                flash('success', "Uporabnik {$u['username']} odstranjen.");
            }
            break;

        case 'wifi':
            if (!can('wifi.edit')) deny();
            $old = json_decode((string)$door['wifi_json'], true) ?: [];
            $nets = [];
            for ($i = 0; $i < 3; $i++) {
                $ssid = trim((string)($_POST["ssid$i"] ?? ''));
                $pass = (string)($_POST["pass$i"] ?? '');
                if ($ssid === '') continue;
                if ($pass === '' && isset($old[$i]) && ($old[$i]['ssid'] ?? '') === $ssid) $pass = (string)($old[$i]['pass'] ?? '');
                if ($pass !== '' && (strlen($pass) < 8 || strlen($pass) > 63)) {
                    flash('danger', "Geslo za $ssid mora imeti 8–63 znakov.");
                    redirect($self);
                }
                $nets[] = ['ssid' => mb_substr($ssid, 0, 32), 'pass' => $pass];
            }
            $managed = !empty($_POST['wifi_managed']);
            if ($managed && !$nets) {
                flash('danger', 'Vpišite vsaj eno omrežje ali izklopite upravljanje Wi-Fi s strežnika.');
                break;
            }
            $setupAp = ($_POST['setup_ap'] ?? 'auto') === 'button' ? 'button' : 'auto';
            q('UPDATE vr_doors SET wifi_json = ?, wifi_rev = ?, setup_ap = ? WHERE id = ?',
              [json_encode($nets, JSON_UNESCAPED_UNICODE), $managed ? max(1, (int)$door['wifi_rev'] + 1) : 0, $setupAp, $id]);
            changed('Wi-Fi nastavitve shranjene');
            break;

        case 'firmware':
            if (!can('firmware.manage')) deny();
            $fwId = (int)($_POST['firmware_id'] ?? 0) ?: null;
            q('UPDATE vr_doors SET firmware_id = ? WHERE id = ?', [$fwId, $id]);
            changed($fwId ? 'Dodeljena programska oprema' : 'Dodelitev programske opreme odstranjena');
            break;

        case 'delete':
            if (!can('device.edit')) deny();
            if (($_POST['confirm'] ?? '') !== $door['name']) {
                flash('danger', 'Za brisanje vpišite točno ime vrat.');
                break;
            }
            log_event(null, (int)$me['id'], 'config', "Vrata {$door['name']} izbrisana ({$me['username']}).");
            q('DELETE FROM vr_doors WHERE id = ?', [$id]);
            flash('success', "Vrata {$door['name']} so izbrisana. Naprava se bo ob naslednjem preverjanju znova prikazala kot nova.");
            redirect('index.php');
    }
    redirect($self);
}

// -------------------------------------------------------------- prikaz -----
$r = door_report($door);
$group = $door['group_id'] ? q('SELECT * FROM vr_groups WHERE id = ?', [$door['group_id']])->fetch() : null;

page_start('Vrata ' . $door['name'], 0);
echo '<p><a href="index.php">&larr; Vsa vrata</a></p>';
echo '<div class="row"><div class="col-lg-6">';

// Stanje
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Stanje</h5><table class="table table-sm mb-0">';
echo '<tr><th style="width:40%">Povezava</th><td>' . online_badge($door) . '<br><small>zadnji stik: ' . h(fmt_time($door['last_seen_at'])) . '</small></td></tr>';
echo '<tr><th>Vrata</th><td>' . door_state_html($door) . '</td></tr>';
echo '<tr><th>Status</th><td>' . status_badge((int)$door['status']) . '</td></tr>';
if (!empty($r['event']['summary'])) {
    echo '<tr><th>Trenutni dogodek</th><td>' . h($r['event']['summary']) . ' do ' . h(fmt_ts((int)($r['event']['end'] ?? 0))) . '</td></tr>';
}
if (!empty($r['next']['summary'])) {
    echo '<tr><th>Naslednji dogodek</th><td>' . h($r['next']['summary']) . ' ob ' . h(fmt_ts((int)($r['next']['start'] ?? 0))) . '</td></tr>';
}
if ($door['open_until'] && utc_ts($door['open_until']) > time()) {
    echo '<tr><th>Takojšnji odklep</th><td>do ' . h(fmt_time($door['open_until'], 'H:i')) . '</td></tr>';
}
$applied = (int)($r['config_rev'] ?? 0);
if ($door['mac'] && $applied && $applied < (int)$door['config_rev']) {
    echo '<tr><th>Nastavitve</th><td><span class="badge badge-warning">čakajo na prenos</span></td></tr>';
}
echo '</table>';
if (can('monitoring.view')) echo '<a class="small" href="monitoring.php#d' . $id . '">Podrobno v nadzoru</a> · ';
if (can('log.view')) echo '<a class="small" href="log.php?door=' . $id . '">Dnevnik vrat</a>';
echo '</div></div>';

// Status in takojšnji odklep
$st = can('status.edit');
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Status</h5>'
   . '<form method="post" class="form-inline mb-2">' . csrf_field() . '<input type="hidden" name="section" value="status">'
   . '<select name="status" class="form-control mr-2"' . disabled_unless($st) . '>' . select_options(STATUS_LABELS, (int)$door['status']) . '</select>'
   . '<button class="btn btn-primary"' . disabled_unless($st) . '>Shrani</button></form>'
   . '<form method="post" class="form-inline">' . csrf_field() . '<input type="hidden" name="section" value="open">'
   . 'Odkleni zdaj za&nbsp;<input type="number" name="minutes" value="10" min="1" max="720" class="form-control form-control-sm" style="width:80px"' . disabled_unless($st) . '>&nbsp;min&nbsp;'
   . '<button class="btn btn-sm btn-success mr-2"' . disabled_unless($st) . '>Odkleni</button>'
   . '<button class="btn btn-sm btn-outline-secondary" name="section" value="open_cancel"' . disabled_unless($st) . '>Prekliči odklep</button></form>'
   . '<p class="small text-muted mt-2 mb-0">Odklep deluje v vsakem statusu, tudi pri LOCK. Vrata ga izvedejo ob naslednjem preverjanju (do '
   . (int)$door['poll_s'] . ' s).</p></div></div>';

// Osnovno
$de = can('device.edit');
$groupOpts = ['' => '– brez skupine –'] + array_column(groups_all(), 'name', 'id');
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Osnovno</h5><form method="post">' . csrf_field()
   . '<input type="hidden" name="section" value="basic">'
   . '<div class="form-group"><label>Ime naprave (npr. V1)</label><input class="form-control" name="name" value="' . h($door['name']) . '"' . disabled_unless($de) . '></div>'
   . '<div class="form-group"><label>Opis</label><input class="form-control" name="description" value="' . h($door['description']) . '"' . disabled_unless($de) . '></div>'
   . '<div class="form-group"><label>Skupina (koledar za status 2)</label><select class="form-control" name="group_id"' . disabled_unless($de) . '>'
   . select_options($groupOpts, (string)$door['group_id']) . '</select></div>'
   . '<button class="btn btn-primary"' . disabled_unless($de) . '>Shrani</button></form>'
   . '<p class="small text-muted mt-2 mb-0">MAC: ' . h($door['mac'] ?: '–') . '</p></div></div>';

// Koledar
$ce = can('calendar.edit');
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Koledar</h5>'
   . '<p class="small">Koledar skupine (status 2): ' . ($group ? '<b>' . h($group['name']) . '</b>' . ($group['disabled'] ? ' (onemogočena)' : '') : '–') . '</p>'
   . '<form method="post">' . csrf_field() . '<input type="hidden" name="section" value="calendar">'
   . '<div class="form-group"><label>Lokalni koledar (status 3) – tajni naslov iCal</label>'
   . '<input class="form-control" name="local_ical_url" value="' . h($ce ? $door['local_ical_url'] : ($door['local_ical_url'] ? '(nastavljen)' : '')) . '"' . disabled_unless($ce) . '></div>'
   . '<button class="btn btn-primary"' . disabled_unless($ce) . '>Shrani</button></form></div></div>';

echo '</div><div class="col-lg-6">';

// Portal telovadnice
$pc = can('portal.config');
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Portal (odklep s PIN-om prek Wi-Fi)</h5>'
   . '<form method="post">' . csrf_field() . '<input type="hidden" name="section" value="portal">'
   . '<div class="form-check mb-2"><input class="form-check-input" type="checkbox" name="portal_enabled" id="pe" value="1"'
   . ($door['portal_enabled'] ? ' checked' : '') . disabled_unless($pc) . '><label class="form-check-label" for="pe">Vklopljen (vrata oddajajo odprto omrežje)</label></div>'
   . '<div class="form-row"><div class="form-group col-6"><label>Ime omrežja</label><input class="form-control" name="portal_ssid" maxlength="32" value="' . h($door['portal_ssid']) . '"' . disabled_unless($pc) . '></div>'
   . '<div class="form-group col-6"><label>Odklep traja (s)</label><input class="form-control" type="number" min="10" max="3600" name="portal_unlock_s" value="' . (int)$door['portal_unlock_s'] . '"' . disabled_unless($pc) . '></div></div>'
   . '<div class="form-group"><label>Omrežje po odklepu izklopi za (s)</label><input class="form-control" type="number" min="0" max="3600" name="portal_pause_s" value="'
   . (int)($door['portal_pause_s'] ?? max(0, (int)$door['portal_unlock_s'] - 60)) . '"' . disabled_unless($pc) . '>'
   . '<small class="form-text text-muted">Telefoni in tablice se med tem ne morejo znova povezati. 0 = omrežje ostane vklopljeno. '
   . 'Priporočeno: čas odklepa − 60 s, da je omrežje nazaj minuto pred zaklepom. Največ toliko, kot traja odklep.</small></div>'
   . '<div class="form-row"><div class="form-group col-6"><label>PIN velja pred začetkom (min)</label><input class="form-control" type="number" min="0" max="240" name="pin_before_min" value="' . (int)$door['pin_before_min'] . '"' . disabled_unless($pc) . '></div>'
   . '<div class="form-group col-6"><label>in po koncu rezervacije (min)</label><input class="form-control" type="number" min="0" max="240" name="pin_after_min" value="' . (int)$door['pin_after_min'] . '"' . disabled_unless($pc) . '></div></div>'
   . '<button class="btn btn-primary"' . disabled_unless($pc) . '>Shrani</button></form>'
   . '<p class="small text-muted mt-2 mb-0">Rezervacije: dogodek v koledarju z naslovom <code>Ime Priimek-123456</code> '
   . '(PIN 4–8 številk) da uporabniku <code>imepriimek</code> PIN v času rezervacije. Takšni dogodki vrat ne odprejo sami. '
   . 'Status LOCK onemogoči vse PIN-e.</p></div></div>';

if (can('portal.users')) {
    $users = q('SELECT * FROM vr_portal_users WHERE door_id = ? ORDER BY username', [$id])->fetchAll();
    echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Stalni uporabniki portala</h5>'
       . '<p class="small text-muted">PIN velja ves čas (razen pri statusu LOCK). Za rezervacije uporabite koledar.</p>';
    if ($users) {
        echo '<table class="table table-sm"><tr><th>Uporabniško ime</th><th>Ime</th><th>PIN</th><th></th></tr>';
        foreach ($users as $u) {
            echo '<tr><td><code>' . h($u['username']) . '</code></td><td>' . h($u['display_name']) . '</td><td class="text-monospace">' . h($u['pin'])
               . '</td><td><form method="post" onsubmit="return confirm(\'Odstranim?\')">' . csrf_field()
               . '<input type="hidden" name="section" value="portal_del"><input type="hidden" name="user_id" value="' . (int)$u['id'] . '">'
               . '<button class="btn btn-sm btn-outline-danger">Odstrani</button></form></td></tr>';
        }
        echo '</table>';
    }
    echo '<form method="post" class="form-row align-items-end">' . csrf_field() . '<input type="hidden" name="section" value="portal_add">'
       . '<div class="col-6"><label class="small">Ime in priimek</label><input class="form-control form-control-sm" name="display_name" required></div>'
       . '<div class="col-3"><label class="small">PIN (prazno = naključen)</label><input class="form-control form-control-sm" name="pin" inputmode="numeric" pattern="[0-9]{4,8}"></div>'
       . '<div class="col-3"><button class="btn btn-sm btn-success">Dodaj</button></div></form>'
       . '<p class="small text-muted mt-2 mb-0">Uporabniško ime nastane iz imena: male črke, brez presledkov in šumnikov (Ana Šolar → anasolar).</p></div></div>';
}

// Intervali
$te = can('timing.edit');
echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Intervali</h5><form method="post" class="form-row align-items-end">' . csrf_field()
   . '<input type="hidden" name="section" value="timing">'
   . '<div class="col-5"><label class="small">Preverjanje strežnika (' . POLL_MIN . '–' . POLL_MAX . ' s)</label><input class="form-control" type="number" name="poll_s" min="' . POLL_MIN . '" max="' . POLL_MAX . '" value="' . (int)$door['poll_s'] . '"' . disabled_unless($te) . '></div>'
   . '<div class="col-5"><label class="small">Prenos koledarja (' . CAL_MIN . '–' . CAL_MAX . ' s)</label><input class="form-control" type="number" name="calendar_s" min="' . CAL_MIN . '" max="' . CAL_MAX . '" value="' . (int)$door['calendar_s'] . '"' . disabled_unless($te) . '></div>'
   . '<div class="col-2"><button class="btn btn-primary"' . disabled_unless($te) . '>Shrani</button></div></form></div></div>';

// Wi-Fi
if (can('wifi.edit')) {
    $nets = json_decode((string)$door['wifi_json'], true) ?: [];
    echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Wi-Fi naprave</h5><form method="post">' . csrf_field()
       . '<input type="hidden" name="section" value="wifi">'
       . '<div class="form-check mb-2"><input class="form-check-input" type="checkbox" id="wm" name="wifi_managed" value="1"' . ((int)$door['wifi_rev'] > 0 ? ' checked' : '')
       . '><label class="form-check-label" for="wm">Wi-Fi omrežja nastavlja strežnik (sicer ostanejo tista na napravi)</label></div>';
    for ($i = 0; $i < 3; $i++) {
        $n = $nets[$i] ?? ['ssid' => '', 'pass' => ''];
        echo '<div class="form-row"><div class="form-group col-6"><input class="form-control form-control-sm" name="ssid' . $i . '" placeholder="Omrežje ' . ($i + 1) . '" value="' . h($n['ssid']) . '"></div>'
           . '<div class="form-group col-6"><input class="form-control form-control-sm" type="password" name="pass' . $i . '" placeholder="' . ($n['pass'] !== '' ? '(nespremenjeno)' : 'geslo') . '" autocomplete="new-password"></div></div>';
    }
    echo '<div class="form-group"><label class="small">Nastavitveno omrežje VRATA-SETUP</label><select class="form-control form-control-sm" name="setup_ap">'
       . select_options(['auto' => 'samodejno, ko Wi-Fi ne deluje 1 min', 'button' => 'samo s kratkim pritiskom na BOOT (10 min)'], $door['setup_ap']) . '</select></div>'
       . '<button class="btn btn-primary">Shrani</button></form>'
       . '<p class="small text-muted mt-2 mb-0">Če se naprava z novimi omrežji ne poveže v 5 minutah, se vrne na prejšnja. '
       . 'Pri vklopljenem portalu je nastavitveno omrežje dosegljivo samo s pritiskom na BOOT.</p></div></div>';
}

// Programska oprema
if (can('firmware.manage')) {
    $fwOpts = ['' => '– brez (ostane trenutna) –'] + array_column(
        q("SELECT id, CONCAT(version, ' (', DATE_FORMAT(uploaded_at, '%d.%m.%Y'), ')') AS label FROM vr_firmware ORDER BY id DESC")->fetchAll(),
        'label', 'id');
    echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">Programska oprema</h5>'
       . '<p class="small mb-2">Na napravi: <b>' . h($door['fw_version'] ?: '–') . '</b>'
       . (!empty($r['ota']['bad']) ? ' · <span class="text-danger">zavrnjena različica ' . h($r['ota']['bad']) . '</span>' : '') . '</p>'
       . '<form method="post" class="form-inline">' . csrf_field() . '<input type="hidden" name="section" value="firmware">'
       . '<select class="form-control form-control-sm mr-2" name="firmware_id">' . select_options($fwOpts, (string)$door['firmware_id']) . '</select>'
       . '<button class="btn btn-sm btn-warning">Dodeli</button></form></div></div>';
}

// Brisanje
if (can('device.edit')) {
    echo '<div class="card mb-3 border-danger"><div class="card-body"><h5 class="card-title text-danger">Izbriši vrata</h5>'
       . '<form method="post" class="form-inline">' . csrf_field() . '<input type="hidden" name="section" value="delete">'
       . '<input class="form-control form-control-sm mr-2" name="confirm" placeholder="vpišite ' . h($door['name']) . '">'
       . '<button class="btn btn-sm btn-danger">Izbriši</button></form>'
       . '<p class="small text-muted mt-2 mb-0">Naprava ostane v zadnjem stanju in se znova prikaže med novimi.</p></div></div>';
}
echo '</div></div>';
page_end();
