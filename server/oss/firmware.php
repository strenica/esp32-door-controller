<?php
// Programska oprema: nalaganje .bin datotek in dodeljevanje vratom.
// Vrata dodeljeno različico prenesejo sama; če nova različica v 10 minutah
// ne vzpostavi stika s strežnikom, se naprava sama vrne na prejšnjo.
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('firmware.manage');

[$scope, $scopeParams] = door_scope('d');

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? '');

    if ($action === 'upload') {
        $f = $_FILES['bin'] ?? null;
        if (!$f || $f['error'] !== UPLOAD_ERR_OK) {
            flash('danger', 'Nalaganje ni uspelo (koda ' . (int)($f['error'] ?? -1) . '). Preverite upload_max_filesize v nastavitvah PHP.');
            redirect('firmware.php');
        }
        $data = file_get_contents($f['tmp_name']);
        if ($data === false || strlen($data) < 100000 || strlen($data) > 4 * 1024 * 1024 || ord($data[0]) !== 0xE9) {
            flash('danger', 'To ni datoteka programske opreme ESP32 (pričakovana je vrata.ino.bin, ne .merged.bin).');
            redirect('firmware.php');
        }
        if (!preg_match('/VRATA_FW_VERSION=([0-9A-Za-z._-]{1,32});/', $data, $m)) {
            flash('danger', 'V datoteki ni oznake različice – to ni programska oprema za Vrata.');
            redirect('firmware.php');
        }
        $version = $m[1];
        q('INSERT INTO vr_firmware (version, filename, size, md5, data, notes, uploaded_by, uploaded_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
          [$version, mb_substr(basename((string)$f['name']), 0, 255), strlen($data), md5($data), $data,
           mb_substr(trim((string)($_POST['notes'] ?? '')), 0, 255), (int)$me['id'], now_utc()]);
        log_event(null, (int)$me['id'], 'ota', "Naložena programska oprema $version ({$me['username']}).");
        flash('success', "Različica $version je naložena. Dodelite jo vratom spodaj.");
        redirect('firmware.php');
    }

    $fwId = (int)($_POST['id'] ?? 0);
    $fw = q('SELECT id, version FROM vr_firmware WHERE id = ?', [$fwId])->fetch();
    if (!$fw) redirect('firmware.php');

    if ($action === 'assign') {
        $ids = !empty($_POST['all'])
            ? q("SELECT d.id FROM vr_doors d WHERE $scope", $scopeParams)->fetchAll(PDO::FETCH_COLUMN)
            : array_map('intval', (array)($_POST['doors'] ?? []));
        $ids = array_values(array_filter($ids, fn($i) => can_see_door(['id' => $i, 'group_id' => q('SELECT group_id FROM vr_doors WHERE id = ?', [$i])->fetchColumn()])));
        foreach ($ids as $i) q('UPDATE vr_doors SET firmware_id = ? WHERE id = ?', [$fwId, $i]);
        bump_rev($ids);
        log_event(null, (int)$me['id'], 'ota', "Različica {$fw['version']} dodeljena " . count($ids) . " vratom ({$me['username']}).");
        flash('success', "Različica {$fw['version']} je dodeljena " . count($ids) . ' vratom. Namestijo jo ob naslednjem preverjanju.');
    } elseif ($action === 'delete') {
        q('DELETE FROM vr_firmware WHERE id = ?', [$fwId]);
        log_event(null, (int)$me['id'], 'ota', "Različica {$fw['version']} izbrisana ({$me['username']}).");
        flash('success', "Različica {$fw['version']} je izbrisana.");
    }
    redirect('firmware.php');
}

$list = q('SELECT f.id, f.version, f.filename, f.size, f.md5, f.notes, f.uploaded_at, u.username,
                  (SELECT COUNT(*) FROM vr_doors d WHERE d.firmware_id = f.id) AS assigned
           FROM vr_firmware f LEFT JOIN vr_users u ON u.id = f.uploaded_by ORDER BY f.id DESC')->fetchAll();
$doors = q("SELECT d.id, d.name, d.fw_version, d.firmware_id FROM vr_doors d WHERE $scope ORDER BY d.name", $scopeParams)->fetchAll();

page_start('Programska oprema');
echo '<div class="card mb-4"><div class="card-body"><h5 class="card-title">Naloži novo različico</h5>'
   . '<p class="small text-muted">V Arduino IDE: <i>Sketch → Export Compiled Binary</i>, nato naložite <code>vrata.ino.bin</code> '
   . '(ne <code>.merged.bin</code>). Različica se prebere iz datoteke. Največja velikost nalaganja na tem strežniku: '
   . h(ini_get('upload_max_filesize')) . '.</p>'
   . '<form method="post" enctype="multipart/form-data" class="form-inline">' . csrf_field()
   . '<input type="file" name="bin" accept=".bin" class="form-control-file mr-2" required>'
   . '<input name="notes" class="form-control form-control-sm mr-2" placeholder="opomba (neobvezno)">'
   . '<button class="btn btn-primary btn-sm" name="action" value="upload">Naloži</button></form></div></div>';

foreach ($list as $f) {
    echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">' . h($f['version'])
       . ' <small class="text-muted">' . h($f['filename']) . ' · ' . round($f['size'] / 1024) . ' KB · ' . h(fmt_time($f['uploaded_at']))
       . ' · ' . h($f['username'] ?? '') . '</small></h5>'
       . ($f['notes'] ? '<p class="small">' . h($f['notes']) . '</p>' : '')
       . '<form method="post">' . csrf_field() . '<input type="hidden" name="id" value="' . (int)$f['id'] . '"><div class="mb-2">';
    foreach ($doors as $d) {
        $checked = (int)$d['firmware_id'] === (int)$f['id'] ? ' checked' : '';
        echo '<label class="mr-3 small"><input type="checkbox" name="doors[]" value="' . (int)$d['id'] . '"' . $checked . '> '
           . h($d['name']) . ' <span class="text-muted">(' . h($d['fw_version'] ?: '?') . ')</span></label>';
    }
    echo '</div><button class="btn btn-sm btn-warning" name="action" value="assign">Dodeli izbranim</button> '
       . '<button class="btn btn-sm btn-outline-warning" name="action" value="assign" onclick="this.form.all.value=1">Dodeli vsem</button> '
       . '<input type="hidden" name="all" value="">'
       . '<button class="btn btn-sm btn-outline-danger float-right" name="action" value="delete" onclick="return confirm(\'Izbrišem to različico?\')">Izbriši</button>'
       . '</form><p class="small text-muted mt-2 mb-0">Dodeljeno vratom: ' . (int)$f['assigned'] . '. Dodelitev odstranite na strani posameznih vrat.</p></div></div>';
}
if (!$list) echo '<p class="text-muted">Ni naloženih različic.</p>';
page_end();
