<?php
// Dnevnik dogodkov: odklepi, spremembe nastavitev, zagoni, napake ...
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('log.view');

const EVENT_TYPES = [
    '' => 'vsi', 'unlock' => 'odklepi', 'status' => 'status', 'mode' => 'način vrat', 'config' => 'nastavitve',
    'portal' => 'portal', 'boot' => 'zagoni', 'calendar' => 'koledar', 'error' => 'napake', 'offline_edit' => 'spremembe brez povezave',
    'wifi' => 'Wi-Fi', 'ota' => 'posodobitve', 'adopt' => 'sprejem', 'user' => 'uporabniki',
];

$doorId = (int)($_GET['door'] ?? 0);
$type = (string)($_GET['type'] ?? '');
if (!isset(EVENT_TYPES[$type])) $type = '';
$page = max(1, (int)($_GET['page'] ?? 1));
$perPage = 100;

[$scope, $params] = door_scope('d');
$where = ["($scope" . ($me['all_doors'] ? ' OR e.door_id IS NULL' : '') . ')'];
if ($doorId) {
    $where[] = 'e.door_id = ?';
    $params[] = $doorId;
}
if ($type !== '') {
    $where[] = 'e.type = ?';
    $params[] = $type;
}
$sql = 'SELECT e.*, d.name AS door_name, u.username FROM vr_events e
        LEFT JOIN vr_doors d ON d.id = e.door_id LEFT JOIN vr_users u ON u.id = e.user_id
        WHERE ' . implode(' AND ', $where) . ' ORDER BY e.id DESC LIMIT ' . ($perPage + 1) . ' OFFSET ' . (($page - 1) * $perPage);
$rows = q($sql, $params)->fetchAll();
$hasMore = count($rows) > $perPage;
$rows = array_slice($rows, 0, $perPage);

[$scope2, $params2] = door_scope('d');
$doorOpts = ['0' => 'vsa vrata'] + array_column(q("SELECT d.id, d.name FROM vr_doors d WHERE $scope2 ORDER BY d.name", $params2)->fetchAll(), 'name', 'id');

page_start('Dnevnik');
echo '<form class="form-inline mb-3" method="get"><select name="door" class="form-control form-control-sm mr-2">' . select_options($doorOpts, (string)$doorId)
   . '</select><select name="type" class="form-control form-control-sm mr-2">' . select_options(EVENT_TYPES, $type)
   . '</select><button class="btn btn-sm btn-primary">Prikaži</button></form>';
echo '<div class="table-responsive"><table class="table table-sm table-striped bg-white small"><tr><th>Čas</th><th>Vrata</th><th>Vrsta</th><th>Dogodek</th></tr>';
foreach ($rows as $e) {
    echo '<tr><td class="text-nowrap">' . h(fmt_time($e['created_at'])) . '</td><td>' . h($e['door_name'] ?? '–') . '</td><td>'
       . h(EVENT_TYPES[$e['type']] ?? $e['type']) . '</td><td>' . h($e['message']) . '</td></tr>';
}
if (!$rows) echo '<tr><td colspan="4" class="text-muted">Ni dogodkov.</td></tr>';
echo '</table></div>';
$qs = fn($p) => 'log.php?' . http_build_query(['door' => $doorId, 'type' => $type, 'page' => $p]);
if ($page > 1) echo '<a class="btn btn-sm btn-outline-secondary mr-2" href="' . h($qs($page - 1)) . '">&larr; Novejši</a>';
if ($hasMore) echo '<a class="btn btn-sm btn-outline-secondary" href="' . h($qs($page + 1)) . '">Starejši &rarr;</a>';
page_end();
