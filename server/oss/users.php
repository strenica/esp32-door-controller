<?php
// Uporabniki spletne strani: vloga (pravice) in omejitev na skupine/vrata.
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('users.manage');

function admin_count_without(int $excludeId): int
{
    return (int)q('SELECT COUNT(*) FROM vr_users u JOIN vr_roles r ON r.id = u.role_id
                   WHERE r.is_system = 1 AND u.active = 1 AND u.all_doors = 1 AND u.id <> ?', [$excludeId])->fetchColumn();
}

$roles = array_column(q('SELECT id, name FROM vr_roles ORDER BY id')->fetchAll(), 'name', 'id');
$groups = array_column(groups_all(), 'name', 'id');
$doorList = array_column(q('SELECT id, name FROM vr_doors ORDER BY name')->fetchAll(), 'name', 'id');

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? '');
    $uid = (int)($_POST['id'] ?? 0);

    if ($action === 'save') {
        $username = trim((string)($_POST['username'] ?? ''));
        $roleId = (int)($_POST['role_id'] ?? 0);
        $active = !empty($_POST['active']) ? 1 : 0;
        $allDoors = ($_POST['scope'] ?? 'all') === 'all' ? 1 : 0;
        $password = (string)($_POST['password'] ?? '');
        $isSystemRole = (bool)q('SELECT is_system FROM vr_roles WHERE id = ?', [$roleId])->fetchColumn();

        if (!preg_match('/^[A-Za-z0-9._-]{3,64}$/', $username)) {
            flash('danger', 'Uporabniško ime: 3–64 znakov (črke, številke, . _ -).');
        } elseif (!isset($roles[$roleId])) {
            flash('danger', 'Izberite vlogo.');
        } elseif (q('SELECT id FROM vr_users WHERE username = ? AND id <> ?', [$username, $uid])->fetch()) {
            flash('danger', 'To uporabniško ime že obstaja.');
        } elseif (!$uid && mb_strlen($password) < 10) {
            flash('danger', 'Novi uporabnik potrebuje geslo z vsaj 10 znaki.');
        } elseif ($uid && $password !== '' && mb_strlen($password) < 10) {
            flash('danger', 'Geslo mora imeti vsaj 10 znakov.');
        } elseif ($uid && (!$active || !$isSystemRole || !$allDoors) && admin_count_without($uid) === 0
                  && (bool)q('SELECT r.is_system FROM vr_users u JOIN vr_roles r ON r.id = u.role_id WHERE u.id = ?', [$uid])->fetchColumn()) {
            flash('danger', 'Ostati mora vsaj en aktiven Administrator z dostopom do vseh vrat.');
        } else {
            $display = mb_substr(trim((string)($_POST['display_name'] ?? '')), 0, 100);
            if ($uid) {
                q('UPDATE vr_users SET username = ?, display_name = ?, role_id = ?, active = ?, all_doors = ? WHERE id = ?',
                  [$username, $display, $roleId, $active, $allDoors, $uid]);
                if ($password !== '') q('UPDATE vr_users SET password_hash = ? WHERE id = ?', [password_hash($password, PASSWORD_DEFAULT), $uid]);
            } else {
                q('INSERT INTO vr_users (username, display_name, password_hash, role_id, all_doors, active, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)',
                  [$username, $display, password_hash($password, PASSWORD_DEFAULT), $roleId, $allDoors, $active, now_utc()]);
                $uid = (int)db()->lastInsertId();
            }
            q('DELETE FROM vr_user_groups WHERE user_id = ?', [$uid]);
            q('DELETE FROM vr_user_doors WHERE user_id = ?', [$uid]);
            if (!$allDoors) {
                foreach ((array)($_POST['groups'] ?? []) as $g) if (isset($groups[(int)$g])) q('INSERT INTO vr_user_groups VALUES (?, ?)', [$uid, (int)$g]);
                foreach ((array)($_POST['doors'] ?? []) as $d) if (isset($doorList[(int)$d])) q('INSERT INTO vr_user_doors VALUES (?, ?)', [$uid, (int)$d]);
            }
            log_event(null, (int)$me['id'], 'user', "Uporabnik $username shranjen ({$me['username']}).");
            flash('success', "Uporabnik $username je shranjen.");
            redirect('users.php');
        }
        redirect('users.php' . ($uid ? '?edit=' . $uid : '?new=1'));
    }

    if ($action === 'delete' && $uid) {
        if ($uid === (int)$me['id']) {
            flash('danger', 'Samega sebe ne morete izbrisati.');
        } elseif (admin_count_without($uid) === 0) {
            flash('danger', 'Ostati mora vsaj en aktiven Administrator.');
        } else {
            $name = q('SELECT username FROM vr_users WHERE id = ?', [$uid])->fetchColumn();
            q('DELETE FROM vr_users WHERE id = ?', [$uid]);
            log_event(null, (int)$me['id'], 'user', "Uporabnik $name izbrisan ({$me['username']}).");
            flash('success', "Uporabnik $name je izbrisan.");
        }
        redirect('users.php');
    }
}

page_start('Uporabniki');
$editId = (int)($_GET['edit'] ?? 0);
if ($editId || isset($_GET['new'])) {
    $u = $editId ? q('SELECT * FROM vr_users WHERE id = ?', [$editId])->fetch() : null;
    $u = $u ?: ['id' => 0, 'username' => '', 'display_name' => '', 'role_id' => 2, 'active' => 1, 'all_doors' => 1];
    $ug = $u['id'] ? q('SELECT group_id FROM vr_user_groups WHERE user_id = ?', [$u['id']])->fetchAll(PDO::FETCH_COLUMN) : [];
    $ud = $u['id'] ? q('SELECT door_id FROM vr_user_doors WHERE user_id = ?', [$u['id']])->fetchAll(PDO::FETCH_COLUMN) : [];
    echo '<form method="post" class="card card-body mb-4" style="max-width:720px">' . csrf_field()
       . '<input type="hidden" name="id" value="' . (int)$u['id'] . '"><h5>' . ($u['id'] ? 'Uredi ' . h($u['username']) : 'Nov uporabnik') . '</h5>'
       . '<div class="form-row"><div class="form-group col-md-6"><label>Uporabniško ime</label><input class="form-control" name="username" value="' . h($u['username']) . '"></div>'
       . '<div class="form-group col-md-6"><label>Ime in priimek</label><input class="form-control" name="display_name" value="' . h($u['display_name']) . '"></div></div>'
       . '<div class="form-row"><div class="form-group col-md-6"><label>Vloga</label><select class="form-control" name="role_id">' . select_options($roles, $u['role_id']) . '</select></div>'
       . '<div class="form-group col-md-6"><label>' . ($u['id'] ? 'Novo geslo (prazno = brez spremembe)' : 'Geslo (vsaj 10 znakov)') . '</label>'
       . '<input class="form-control" type="password" name="password" autocomplete="new-password"></div></div>'
       . '<div class="form-check mb-3"><input class="form-check-input" type="checkbox" name="active" value="1" id="ac"' . ($u['active'] ? ' checked' : '') . '><label class="form-check-label" for="ac">Aktiven</label></div>'
       . '<label>Dostop do vrat</label><div class="mb-2">'
       . '<label class="mr-3"><input type="radio" name="scope" value="all"' . ($u['all_doors'] ? ' checked' : '') . '> vsa vrata</label>'
       . '<label><input type="radio" name="scope" value="some"' . (!$u['all_doors'] ? ' checked' : '') . '> samo izbrane skupine in vrata:</label></div>'
       . '<div class="mb-2"><b class="small">Skupine:</b> ';
    foreach ($groups as $gid => $gname) {
        echo '<label class="mr-3 small"><input type="checkbox" name="groups[]" value="' . (int)$gid . '"' . (in_array($gid, $ug) ? ' checked' : '') . '> ' . h($gname) . '</label>';
    }
    echo '</div><div class="mb-3"><b class="small">Posamezna vrata:</b> ';
    foreach ($doorList as $did => $dname) {
        echo '<label class="mr-3 small"><input type="checkbox" name="doors[]" value="' . (int)$did . '"' . (in_array($did, $ud) ? ' checked' : '') . '> ' . h($dname) . '</label>';
    }
    echo '</div><div><button class="btn btn-primary" name="action" value="save">Shrani</button> <a class="btn btn-link" href="users.php">Prekliči</a>';
    if ($u['id'] && (int)$u['id'] !== (int)$me['id']) {
        echo '<button class="btn btn-outline-danger float-right" name="action" value="delete" onclick="return confirm(\'Izbrišem uporabnika?\')">Izbriši</button>';
    }
    echo '</div></form>';
}

$users = q('SELECT u.*, r.name AS role_name FROM vr_users u JOIN vr_roles r ON r.id = u.role_id ORDER BY u.username')->fetchAll();
echo '<a class="btn btn-success btn-sm mb-2" href="users.php?new=1">Nov uporabnik</a>';
echo '<table class="table table-sm bg-white"><tr><th>Uporabnik</th><th>Ime</th><th>Vloga</th><th>Dostop</th><th>Zadnja prijava</th><th></th></tr>';
foreach ($users as $u) {
    if ($u['all_doors']) {
        $scopeText = 'vsa vrata';
    } else {
        $gs = q('SELECT g.name FROM vr_user_groups ug JOIN vr_groups g ON g.id = ug.group_id WHERE ug.user_id = ?', [$u['id']])->fetchAll(PDO::FETCH_COLUMN);
        $ds = q('SELECT d.name FROM vr_user_doors ud JOIN vr_doors d ON d.id = ud.door_id WHERE ud.user_id = ?', [$u['id']])->fetchAll(PDO::FETCH_COLUMN);
        $scopeText = implode(', ', array_merge($gs, $ds)) ?: 'nič';
    }
    echo '<tr' . ($u['active'] ? '' : ' class="text-muted"') . '><td>' . h($u['username']) . ($u['active'] ? '' : ' (neaktiven)') . '</td><td>' . h($u['display_name'])
       . '</td><td>' . h($u['role_name']) . '</td><td>' . h($scopeText) . '</td><td>' . h(fmt_time($u['last_login_at'])) . '</td>'
       . '<td><a class="btn btn-sm btn-outline-primary" href="users.php?edit=' . (int)$u['id'] . '">Uredi</a></td></tr>';
}
echo '</table>';
page_end();
