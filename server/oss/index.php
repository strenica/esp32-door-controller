<?php
require __DIR__ . '/lib/web.php';
$me = require_login();

if (!can('doors.view')) {
    page_start('Vrata');
    echo '<div class="alert alert-info">Vaša vloga nima pravice za ogled vrat. Izberite stran v meniju.</div>';
    page_end();
    exit;
}

$canAdopt = can('device.edit') && $me['all_doors'];

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? '');

    if ($action === 'status' && can('status.edit')) {
        $door = load_door((int)($_POST['door_id'] ?? 0));
        $status = (int)($_POST['status'] ?? -1);
        if (isset(STATUS_LABELS[$status]) && $status !== (int)$door['status']) {
            q('UPDATE vr_doors SET status = ? WHERE id = ?', [$status, $door['id']]);
            bump_rev([$door['id']]);
            log_event((int)$door['id'], (int)$me['id'], 'status', 'Status: ' . STATUS_LABELS[$status] . ' (' . $me['username'] . ')');
            flash('success', $door['name'] . ': status je ' . STATUS_LABELS[$status] . '. Vrata ga dobijo ob naslednjem preverjanju.');
        }
        redirect('index.php');
    }

    if ($action === 'adopt' && $canAdopt) {
        $mac = (string)($_POST['mac'] ?? '');
        $p = q('SELECT * FROM vr_pending WHERE mac = ?', [$mac])->fetch();
        if (!$p) {
            flash('danger', 'Teh vrat ni več na seznamu novih.');
            redirect('index.php');
        }
        // Ključ se izroči samo proti kodi, ki jo je administrator videl in
        // primerjal z napravo - ne proti trenutni vsebini tabele.
        $pairCode = strtoupper((string)($_POST['pair_code'] ?? ''));
        if (!preg_match('/^[A-Z0-9]{4,8}$/', $pairCode)) {
            flash('danger', 'Koda za povezavo ni veljavna. Osvežite stran in poskusite znova.');
            redirect('index.php');
        }
        $token = bin2hex(random_bytes(32));
        $existing = (int)($_POST['existing_id'] ?? 0);
        $groupId = (int)($_POST['group_id'] ?? 0) ?: null;
        if ($existing > 0) {
            $door = q('SELECT * FROM vr_doors WHERE id = ?', [$existing])->fetch();
            if (!$door) redirect('index.php');
            q('UPDATE vr_doors SET mac = NULL WHERE mac = ? AND id <> ?', [$mac, $existing]);
            q('UPDATE vr_doors SET mac = ?, token_hash = ?, token_plain = ?, pair_code = ?, boot_id = NULL WHERE id = ?',
              [$mac, hash('sha256', $token), $token, $pairCode, $existing]);
            bump_rev([$existing]);
            $doorId = $existing;
            $name = $door['name'];
        } else {
            $name = trim((string)($_POST['name'] ?? ''));
            if (!preg_match('/^[A-Za-z0-9_-]{1,32}$/', $name)) {
                flash('danger', 'Ime vrat: 1–32 znakov (črke, številke, - in _), npr. V1.');
                redirect('index.php');
            }
            if (q('SELECT id FROM vr_doors WHERE name = ?', [$name])->fetch()) {
                flash('danger', "Vrata z imenom $name že obstajajo. Izberite \"obstoječa vrata\" za zamenjavo naprave.");
                redirect('index.php');
            }
            q('UPDATE vr_doors SET mac = NULL WHERE mac = ?', [$mac]);
            q('INSERT INTO vr_doors (name, description, group_id, mac, token_hash, token_plain, pair_code, status, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, 2, ?)',
              [$name, trim((string)($_POST['description'] ?? '')), $groupId, $mac, hash('sha256', $token), $token,
               $pairCode, now_utc()]);
            $doorId = (int)db()->lastInsertId();
        }
        log_event($doorId, (int)$me['id'], 'adopt', "Sprejeta naprava $mac (koda $pairCode) kot $name ({$me['username']}).");
        flash('success', "Vrata $name so sprejeta. Ključ dobijo ob naslednjem preverjanju (do 1 minute).");
        redirect('index.php');
    }

    if ($action === 'forget' && $canAdopt) {
        q('DELETE FROM vr_pending WHERE mac = ?', [(string)($_POST['mac'] ?? '')]);
        redirect('index.php');
    }
    redirect('index.php');
}

[$scope, $params] = door_scope('d');
$doors = q("SELECT d.*, g.name AS group_name FROM vr_doors d LEFT JOIN vr_groups g ON g.id = d.group_id
            WHERE $scope ORDER BY d.name", $params)->fetchAll();

page_start('Vrata', 60);

if ($canAdopt) {
    $pending = q('SELECT p.* FROM vr_pending p
                  WHERE NOT EXISTS (SELECT 1 FROM vr_doors d WHERE d.mac = p.mac AND d.token_plain IS NOT NULL)
                  ORDER BY p.last_seen_at DESC')->fetchAll();
    if ($pending) {
        $groups = ['' => '– brez skupine –'] + array_column(groups_all(), 'name', 'id');
        $allDoors = ['0' => '– nova vrata –'] + array_column(q('SELECT id, name FROM vr_doors ORDER BY name')->fetchAll(), 'name', 'id');
        echo '<div class="card border-warning mb-4"><div class="card-body"><h5 class="card-title">Nove naprave čakajo na sprejem</h5>'
           . '<p class="small text-muted">Primerjajte <b>kodo za povezavo</b> s kodo na strani naprave (http://&lt;IP vrat&gt;:5000) '
           . 'ali v serijskem dnevniku. Sprejmite samo, če se ujemata.</p>';
        if (pending_count() >= 160) {
            echo '<div class="alert alert-warning py-2 small">Na seznamu je že ' . pending_count() . ' naprav. '
               . 'Če se prava vrata ne prikažejo, odstranite neznane zapise s <b>Odstrani s seznama</b> in se bodo prijavila znova.</div>';
        }
        foreach ($pending as $p) {
            echo '<form method="post" class="form-row align-items-end border-top pt-2 mt-2">' . csrf_field()
               . '<input type="hidden" name="mac" value="' . h($p['mac']) . '">'
               . '<input type="hidden" name="pair_code" value="' . h($p['pair_code']) . '">'
               . '<div class="col-md-3 small"><b>Koda: <span class="text-monospace">' . h($p['pair_code']) . '</span></b><br>MAC '
               . h($p['mac']) . '<br>IP ' . h($p['local_ip'] ?: '?') . ' · ' . h($p['fw_version']) . '<br>' . h(fmt_ago($p['last_seen_at'])) . '</div>'
               . '<div class="col-md-2"><label class="small">Ime novih vrat</label><input class="form-control form-control-sm" name="name" placeholder="npr. V1"></div>'
               . '<div class="col-md-2"><label class="small">ali zamenjava za</label><select class="form-control form-control-sm" name="existing_id">'
               . select_options($allDoors, '0') . '</select></div>'
               . '<div class="col-md-2"><label class="small">Skupina</label><select class="form-control form-control-sm" name="group_id">'
               . select_options($groups, '') . '</select></div>'
               . '<div class="col-md-3"><button class="btn btn-sm btn-success" name="action" value="adopt">Sprejmi</button> '
               . '<button class="btn btn-sm btn-outline-secondary" name="action" value="forget">Odstrani s seznama</button></div></form>';
        }
        echo '</div></div>';
    }
}

if (!$doors) {
    echo '<div class="alert alert-info">Ni vrat, ki bi jih lahko videli.</div>';
} else {
    echo '<div class="table-responsive"><table class="table table-sm table-hover bg-white"><thead><tr>'
       . '<th>Vrata</th><th>Skupina</th><th>Status</th><th>Vrata zdaj</th><th>Povezava</th><th>Trenutni dogodek</th><th></th></tr></thead><tbody>';
    foreach ($doors as $d) {
        $r = door_report($d);
        $event = '';
        if (!empty($r['event']['summary'])) {
            $event = h($r['event']['summary']) . ' <small class="text-muted">do ' . h(fmt_ts((int)($r['event']['end'] ?? 0), 'H:i')) . '</small>';
        }
        if ($d['open_until'] && utc_ts($d['open_until']) > time()) {
            $event .= ' <span class="badge badge-warning">odklenjeno do ' . h(fmt_time($d['open_until'], 'H:i')) . '</span>';
        }
        echo '<tr><td><b>' . h($d['name']) . '</b><br><small class="text-muted">' . h($d['description']) . '</small>'
           . ($d['portal_enabled'] ? ' <span class="badge badge-light">portal</span>' : '') . '</td>'
           . '<td>' . h($d['group_name'] ?? '–') . '</td><td>';
        if (can('status.edit')) {
            echo '<form method="post" class="form-inline">' . csrf_field()
               . '<input type="hidden" name="action" value="status"><input type="hidden" name="door_id" value="' . (int)$d['id'] . '">'
               . '<select name="status" class="form-control form-control-sm mr-1" onchange="this.form.submit()">'
               . select_options(STATUS_LABELS, (int)$d['status']) . '</select></form>';
        } else {
            echo status_badge((int)$d['status']);
        }
        echo '</td><td>' . door_state_html($d) . '</td><td>' . online_badge($d) . '</td><td>' . ($event ?: '–') . '</td>'
           . '<td><a class="btn btn-sm btn-outline-primary" href="door.php?id=' . (int)$d['id'] . '">Podrobnosti</a></td></tr>';
    }
    echo '</tbody></table></div>';
    echo '<p class="small text-muted">Stran se osveži vsako minuto. Spremembe vrata dobijo ob naslednjem preverjanju strežnika.</p>';
}
page_end();
