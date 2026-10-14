<?php
// Vloge: za vsako vlogo izberete, kaj sme početi. Administrator ima vedno vse.
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('users.manage');

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? '');
    $rid = (int)($_POST['id'] ?? 0);
    $role = $rid ? q('SELECT * FROM vr_roles WHERE id = ?', [$rid])->fetch() : null;
    if ($role && $role['is_system']) deny('Vloge Administrator ni mogoče spreminjati.');

    if ($action === 'save') {
        $name = trim((string)($_POST['name'] ?? ''));
        if ($name === '' || mb_strlen($name) > 64) {
            flash('danger', 'Ime vloge: 1–64 znakov.');
        } elseif (q('SELECT id FROM vr_roles WHERE name = ? AND id <> ?', [$name, $rid])->fetch()) {
            flash('danger', 'Vloga s tem imenom že obstaja.');
        } else {
            $desc = mb_substr(trim((string)($_POST['description'] ?? '')), 0, 255);
            if ($role) {
                q('UPDATE vr_roles SET name = ?, description = ? WHERE id = ?', [$name, $desc, $rid]);
            } else {
                q('INSERT INTO vr_roles (name, description, is_system) VALUES (?, ?, 0)', [$name, $desc]);
                $rid = (int)db()->lastInsertId();
            }
            q('DELETE FROM vr_role_permissions WHERE role_id = ?', [$rid]);
            foreach ((array)($_POST['perms'] ?? []) as $p) {
                if (isset(PERMS[$p])) q('INSERT INTO vr_role_permissions (role_id, perm) VALUES (?, ?)', [$rid, $p]);
            }
            log_event(null, (int)$me['id'], 'user', "Vloga $name shranjena ({$me['username']}).");
            flash('success', "Vloga $name je shranjena.");
        }
    } elseif ($action === 'delete' && $role) {
        $used = (int)q('SELECT COUNT(*) FROM vr_users WHERE role_id = ?', [$rid])->fetchColumn();
        if ($used) {
            flash('danger', "Vlogo uporablja $used uporabnikov. Najprej jim dodelite drugo vlogo.");
        } else {
            q('DELETE FROM vr_roles WHERE id = ?', [$rid]);
            log_event(null, (int)$me['id'], 'user', "Vloga {$role['name']} izbrisana ({$me['username']}).");
            flash('success', "Vloga {$role['name']} je izbrisana.");
        }
    }
    redirect('roles.php');
}

$roles = q('SELECT r.*, (SELECT COUNT(*) FROM vr_users u WHERE u.role_id = r.id) AS users FROM vr_roles r ORDER BY r.id')->fetchAll();
page_start('Vloge');
echo '<p class="text-muted">Pravice določajo, kaj vloga sme. Katera vrata uporabnik vidi, nastavite pri uporabniku (vsa ali izbrane skupine/vrata).</p>';

function role_form(array $r): string
{
    $have = !empty($r['id']) ? q('SELECT perm FROM vr_role_permissions WHERE role_id = ?', [$r['id']])->fetchAll(PDO::FETCH_COLUMN) : [];
    $sys = !empty($r['is_system']);
    $html = '<form method="post">' . csrf_field() . '<input type="hidden" name="id" value="' . (int)($r['id'] ?? 0) . '">'
          . '<div class="form-row"><div class="form-group col-md-4"><input class="form-control form-control-sm" name="name" placeholder="Ime vloge" value="' . h($r['name'] ?? '') . '"' . disabled_unless(!$sys) . '></div>'
          . '<div class="form-group col-md-8"><input class="form-control form-control-sm" name="description" placeholder="Opis" value="' . h($r['description'] ?? '') . '"' . disabled_unless(!$sys) . '></div></div>'
          . '<div class="row">';
    foreach (PERMS as $key => $label) {
        $checked = ($sys || in_array($key, $have, true)) ? ' checked' : '';
        $html .= '<div class="col-md-6"><label class="small"><input type="checkbox" name="perms[]" value="' . h($key) . '"' . $checked . disabled_unless(!$sys) . '> ' . h($label) . '</label></div>';
    }
    $html .= '</div>';
    if (!$sys) {
        $html .= '<button class="btn btn-sm btn-primary" name="action" value="save">Shrani</button> ';
        if (!empty($r['id'])) $html .= '<button class="btn btn-sm btn-outline-danger" name="action" value="delete" onclick="return confirm(\'Izbrišem vlogo?\')">Izbriši</button>';
    } else {
        $html .= '<p class="small text-muted mb-0">Administrator ima vedno vse pravice.</p>';
    }
    return $html . '</form>';
}

foreach ($roles as $r) {
    echo '<div class="card mb-3"><div class="card-body"><h5 class="card-title">' . h($r['name']) . ' <small class="text-muted">' . (int)$r['users'] . ' uporabnikov</small></h5>'
       . role_form($r) . '</div></div>';
}
echo '<div class="card mb-3 border-success"><div class="card-body"><h5 class="card-title">Nova vloga</h5>' . role_form([]) . '</div></div>';
page_end();
