<?php
// Nadzor: vse, kar vrata sporočijo ob vsakem preverjanju.
require __DIR__ . '/lib/web.php';
$me = require_login();
require_perm('monitoring.view');

[$scope, $params] = door_scope('d');
$doors = q("SELECT d.*, g.name AS group_name FROM vr_doors d LEFT JOIN vr_groups g ON g.id = d.group_id
            WHERE $scope ORDER BY d.name", $params)->fetchAll();

page_start('Nadzor', 30);
$online = count(array_filter($doors, 'door_online'));
echo '<p>' . $online . ' od ' . count($doors) . ' vrat je povezanih. Stran se osveži vsakih 30 s.</p>';

function rowf(string $label, string $html): string
{
    return '<tr><th style="width:38%">' . h($label) . '</th><td>' . $html . '</td></tr>';
}

echo '<div class="row">';
foreach ($doors as $d) {
    $r = door_report($d);
    $cal = $r['cal'] ?? [];
    $wifi = $r['wifi'] ?? [];
    $portal = $r['portal'] ?? [];
    $ota = $r['ota'] ?? [];
    echo '<div class="col-xl-4 col-lg-6 mb-3" id="d' . (int)$d['id'] . '"><div class="card h-100"><div class="card-body">'
       . '<h5 class="card-title"><a href="door.php?id=' . (int)$d['id'] . '">' . h($d['name']) . '</a> '
       . '<small class="text-muted">' . h($d['description']) . '</small></h5><table class="table table-sm small mb-0">';
    echo rowf('Povezava', online_badge($d));
    echo rowf('Zadnji stik', h(fmt_time($d['last_seen_at'])) . ' <span class="text-muted">(' . h(fmt_ago($d['last_seen_at'])) . ')</span>');
    echo rowf('Vrata', door_state_html($d));
    echo rowf('Način', h($r['mode_label'] ?? '–') . ' · status strežnika ' . status_badge((int)$d['status']));
    if (!$r) {
        echo '</table></div></div></div>';
        continue;
    }
    echo rowf('Ura naprave', !empty($r['time_valid']) ? 'nastavljena (NTP)' : '<span class="text-danger">ni nastavljena – urnik ne deluje</span>');
    $calHtml = h($cal['source'] ?? '–');
    if (isset($cal['occ'])) $calHtml .= ': ' . (int)$cal['occ'] . ' prihodnjih, ' . (int)($cal['events'] ?? 0) . ' dogodkov v datoteki';
    if (!empty($cal['skipped'])) $calHtml .= ', ' . (int)$cal['skipped'] . ' preskočenih';
    if (!empty($cal['err'])) $calHtml .= '<br><span class="text-danger">' . h($cal['err']) . '</span>';
    echo rowf('Koledar', $calHtml);
    echo rowf('Zadnji prenos', h($cal['last_dl'] ?? '–') . (!empty($cal['last_dl_at']) ? ' <span class="text-muted">(' . h(fmt_ts((int)$cal['last_dl_at'])) . ')</span>' : ''));
    if (!empty($r['event']['summary'])) echo rowf('Trenutni dogodek', h($r['event']['summary']) . ' do ' . h(fmt_ts((int)($r['event']['end'] ?? 0), 'H:i')));
    if (!empty($r['next']['summary'])) echo rowf('Naslednji', h($r['next']['summary']) . ' ' . h(fmt_ts((int)($r['next']['start'] ?? 0))));
    if (!empty($portal['enabled'])) {
        echo rowf('Portal', '„' . h($portal['ssid'] ?? '') . '“ · telefonov: ' . (int)($portal['phones'] ?? 0)
            . (!empty($portal['unlock_left']) ? ' · odklenjeno še ' . (int)$portal['unlock_left'] . ' s' : '')
            . (!empty($portal['paused_left']) ? ' · omrežje izklopljeno še ' . (int)$portal['paused_left'] . ' s' : '')
            . ' · uporabnikov: ' . (int)($portal['users'] ?? 0) . ', rezervacij: ' . (int)($portal['bookings'] ?? 0));
    }
    echo rowf('Wi-Fi', h($wifi['ssid'] ?? '–') . (isset($wifi['rssi']) ? ' · ' . (int)$wifi['rssi'] . ' dBm' : '')
        . (!empty($wifi['ap']) ? ' · <span class="badge badge-warning">nastavitveno omrežje vklopljeno</span>' : '')
        . (!empty($wifi['rejected_rev']) ? '<br><span class="text-danger">nova Wi-Fi nastavitev zavrnjena (povezava ni uspela)</span>' : ''));
    echo rowf('IP', h($r['local_ip'] ?? '–') . ' <span class="text-muted">· javni ' . h($d['last_ip'] ?? '–') . '</span>');
    echo rowf('Programska oprema', h($d['fw_version'] ?: '–')
        . (!empty($ota['pending_verify']) ? ' <span class="badge badge-warning">preverjanje po posodobitvi</span>' : '')
        . (!empty($ota['bad']) ? ' <span class="text-danger">zavrnjena: ' . h($ota['bad']) . '</span>' : '')
        . (!empty($ota['error']) ? '<br><span class="text-danger">' . h($ota['error']) . '</span>' : ''));
    echo rowf('Delovanje', fmt_duration((int)($r['uptime_s'] ?? 0)) . ' · zadnji zagon: ' . h($r['reset_reason'] ?? '–'));
    echo rowf('Pomnilnik', round(((int)($r['heap'] ?? 0)) / 1024) . ' KB prosto (najmanj ' . round(((int)($r['min_heap'] ?? 0)) / 1024) . ' KB)');
    $applied = (int)($r['config_rev'] ?? 0);
    echo rowf('Nastavitve', $applied >= (int)$d['config_rev'] ? 'prenesene' : '<span class="badge badge-warning">čakajo na prenos</span>');
    if (!empty($r['last_error'])) echo rowf('Zadnja napaka', '<span class="text-danger">' . h($r['last_error']) . '</span>');
    echo '</table></div></div></div>';
}
echo '</div>';
page_end();
