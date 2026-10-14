<?php
// Skupine vrat (prej "IOT GROUP"): vsaka ima koledar, ki ga vrata uporabljajo pri statusu 2.
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('groups.edit');

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = (string)($_POST['action'] ?? '');
    $gid = (int)($_POST['id'] ?? 0);
    if ($gid && !can_see_group($gid)) deny();
    $name = trim((string)($_POST['name'] ?? ''));
    $url = trim((string)($_POST['ical_url'] ?? ''));
    $disabled = !empty($_POST['disabled']) ? 1 : 0;

    if ($action === 'save') {
        if ($name === '' || mb_strlen($name) > 64) {
            flash('danger', 'Ime skupine: 1–64 znakov.');
        } elseif ($url !== '' && !preg_match('#^https?://\S+$#i', $url)) {
            flash('danger', 'Naslov koledarja mora začeti s https://');
        } elseif (q('SELECT id FROM vr_groups WHERE name = ? AND id <> ?', [$name, $gid])->fetch()) {
            flash('danger', 'Skupina s tem imenom že obstaja.');
        } elseif ($gid) {
            q('UPDATE vr_groups SET name = ?, ical_url = ?, disabled = ? WHERE id = ?', [$name, $url, $disabled, $gid]);
            bump_rev_group($gid);
            log_event(null, (int)$me['id'], 'config', "Skupina $name shranjena ({$me['username']}).");
            flash('success', "Skupina $name je shranjena.");
        } else {
            if (!$me['all_doors']) deny('Nove skupine lahko ustvari samo uporabnik z dostopom do vseh vrat.');
            q('INSERT INTO vr_groups (name, ical_url, disabled, created_at) VALUES (?, ?, ?, ?)', [$name, $url, $disabled, now_utc()]);
            log_event(null, (int)$me['id'], 'config', "Skupina $name ustvarjena ({$me['username']}).");
            flash('success', "Skupina $name je ustvarjena.");
        }
    } elseif ($action === 'delete' && $gid) {
        $g = q('SELECT name FROM vr_groups WHERE id = ?', [$gid])->fetch();
        bump_rev_group($gid);
        q('DELETE FROM vr_groups WHERE id = ?', [$gid]);
        log_event(null, (int)$me['id'], 'config', "Skupina {$g['name']} izbrisana ({$me['username']}).");
        flash('success', "Skupina {$g['name']} je izbrisana. Njena vrata so zdaj brez skupine.");
    }
    redirect('groups.php');
}

$groups = q("SELECT g.*, (SELECT GROUP_CONCAT(d.name ORDER BY d.name SEPARATOR ', ') FROM vr_doors d WHERE d.group_id = g.id) AS doors
             FROM vr_groups g ORDER BY g.name")->fetchAll();
$groups = array_filter($groups, fn($g) => can_see_group((int)$g['id']));

page_start('Skupine');
echo '<p class="text-muted">Vrata s statusom 2 (urnik skupine) sledijo koledarju svoje skupine. '
   . 'Vpišite <b>tajni naslov v obliki iCal</b> iz nastavitev Google Koledarja.</p>';

function group_form(array $g): string
{
    return '<form method="post" class="form-row align-items-end">' . csrf_field()
        . '<input type="hidden" name="id" value="' . (int)($g['id'] ?? 0) . '">'
        . '<div class="col-md-2"><label class="small">Ime</label><input class="form-control form-control-sm" name="name" value="' . h($g['name'] ?? '') . '"></div>'
        . '<div class="col-md-6"><label class="small">Koledar (iCal)</label><input class="form-control form-control-sm" name="ical_url" value="' . h($g['ical_url'] ?? '') . '"></div>'
        . '<div class="col-md-1"><div class="form-check"><input class="form-check-input" type="checkbox" name="disabled" value="1"' . (!empty($g['disabled']) ? ' checked' : '')
        . '><label class="form-check-label small">onemogočena</label></div></div>'
        . '<div class="col-md-3"><button class="btn btn-sm btn-primary" name="action" value="save">Shrani</button> '
        . (!empty($g['id']) ? '<button class="btn btn-sm btn-outline-danger" name="action" value="delete" onclick="return confirm(\'Izbrišem skupino?\')">Izbriši</button>' : '')
        . '</div></form>';
}

foreach ($groups as $g) {
    echo '<div class="card mb-2"><div class="card-body py-2">' . group_form($g)
       . '<small class="text-muted">Vrata: ' . h($g['doors'] ?: '–') . '</small></div></div>';
}
if ($me['all_doors']) {
    echo '<div class="card mt-4 border-success"><div class="card-body py-2"><h6>Nova skupina</h6>' . group_form([]) . '</div></div>';
}
page_end();
