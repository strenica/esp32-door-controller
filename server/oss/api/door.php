<?php
// API za vrata. Vsaka vrata ga kličejo vsakih poll_s sekund (POST, JSON).
//
// Zahteva:  {"mac":"AA:BB:..","token":"..","pair_code":"K7Q2MX","fw":"2.0.0","boot_id":"..",
//            "reset_reason":"..","local_ip":"..","report":{...},"log":[{"t":..,"type":"..","msg":".."}]}
// Odgovor:  {"ok":true,"state":"active","server_time":..,"config":{...}}   – sprejeta vrata
//           {"ok":true,"state":"adopted","token":".."}                     – ključ po sprejemu
//           {"ok":true,"state":"pending"}                                  – čaka na sprejem
//           {"ok":false,"error":"unauthorized"} (401)                       – ključ ne velja več
require __DIR__ . '/../lib/bootstrap.php';
require __DIR__ . '/../lib/doors.php';

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

function reply(int $code, array $data): never
{
    http_response_code($code);
    echo json_encode($data, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') reply(405, ['ok' => false, 'error' => 'POST only']);
$raw = file_get_contents('php://input', false, null, 0, 65536);
$in = json_decode($raw ?: '', true);
if (!is_array($in)) reply(400, ['ok' => false, 'error' => 'bad json']);

$mac = strtoupper((string)($in['mac'] ?? ''));
if (!preg_match('/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/', $mac)) reply(400, ['ok' => false, 'error' => 'bad mac']);
$token = (string)($in['token'] ?? '');
$fw = mb_substr((string)($in['fw'] ?? ''), 0, 32);
$ip = client_ip();
$now = now_utc();

housekeeping();

// ------------------------------------------------------- sprejeta vrata -----
if ($token !== '') {
    $door = q('SELECT * FROM vr_doors WHERE token_hash = ?', [hash('sha256', $token)])->fetch();
    if (!$door || $door['mac'] !== $mac) reply(401, ['ok' => false, 'error' => 'unauthorized']);
    $doorId = (int)$door['id'];

    $bootId = substr(preg_replace('/[^0-9a-f]/', '', (string)($in['boot_id'] ?? '')), 0, 16);
    if ($bootId !== '' && $bootId !== (string)$door['boot_id']) {
        $why = mb_substr((string)($in['reset_reason'] ?? '?'), 0, 40);
        log_event($doorId, null, 'boot', "Vrata so se zagnala (vzrok: $why, programska oprema $fw).");
    }

    // Dogodki, ki so jih vrata zbrala od zadnjega uspešnega stika (odklepi, spremembe načina ...).
    $log = is_array($in['log'] ?? null) ? array_slice($in['log'], 0, 60) : [];
    foreach ($log as $e) {
        if (!is_array($e)) continue;
        $type = (string)($e['type'] ?? 'info');
        if (!preg_match('/^[a-z_]{2,20}$/', $type)) $type = 'info';
        $t = (int)($e['t'] ?? 0);
        $at = ($t > 1600000000 && $t <= time() + 300) ? gmdate('Y-m-d H:i:s', $t) : $now;
        log_event($doorId, null, $type, (string)($e['msg'] ?? ''), $at);
    }

    $report = is_array($in['report'] ?? null) ? $in['report'] : [];
    $report['local_ip'] = mb_substr((string)($in['local_ip'] ?? ''), 0, 45);
    $report['reset_reason'] = mb_substr((string)($in['reset_reason'] ?? ''), 0, 40);
    $reportJson = json_encode($report, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    if (strlen($reportJson) > 16000) $reportJson = '{}';

    // Pretekel takojšnji odklep pobrišemo, da ga stran ne kaže več.
    $openUntil = ($door['open_until'] && utc_ts($door['open_until']) < time()) ? null : $door['open_until'];

    q('UPDATE vr_doors SET last_seen_at = ?, last_ip = ?, fw_version = ?, boot_id = ?, report = ?, open_until = ?
       WHERE id = ?', [$now, $ip, $fw, $bootId ?: $door['boot_id'], $reportJson, $openUntil, $doorId]);
    $door['open_until'] = $openUntil;

    reply(200, ['ok' => true, 'state' => 'active', 'server_time' => time(), 'config' => door_config($door)]);
}

// ------------------------------------------- nova ali ponastavljena vrata -----
$pair = strtoupper((string)($in['pair_code'] ?? ''));
if (!preg_match('/^[A-Z0-9]{4,8}$/', $pair)) reply(400, ['ok' => false, 'error' => 'bad pair code']);

// Administrator je vrata pravkar sprejel: predamo ključ (samo če se ujema koda za povezavo).
$door = q('SELECT * FROM vr_doors WHERE mac = ?', [$mac])->fetch();
if ($door && $door['token_plain'] && hash_equals((string)$door['pair_code'], $pair)) {
    q('UPDATE vr_doors SET token_plain = NULL, last_seen_at = ?, last_ip = ?, fw_version = ? WHERE id = ?',
      [$now, $ip, $fw, $door['id']]);
    q('DELETE FROM vr_pending WHERE mac = ?', [$mac]);
    log_event((int)$door['id'], null, 'adopt', 'Vrata so prevzela ključ in so povezana s strežnikom.');
    reply(200, ['ok' => true, 'state' => 'adopted', 'token' => $door['token_plain']]);
}

$pendingCount = (int)q('SELECT COUNT(*) FROM vr_pending')->fetchColumn();
$exists = q('SELECT mac FROM vr_pending WHERE mac = ?', [$mac])->fetch();
if ($exists) {
    q('UPDATE vr_pending SET pair_code = ?, ip = ?, local_ip = ?, fw_version = ?, last_seen_at = ? WHERE mac = ?',
      [$pair, $ip, mb_substr((string)($in['local_ip'] ?? ''), 0, 45), $fw, $now, $mac]);
} elseif ($pendingCount < 50) {
    q('INSERT INTO vr_pending (mac, pair_code, ip, local_ip, fw_version, first_seen_at, last_seen_at)
       VALUES (?, ?, ?, ?, ?, ?, ?)',
      [$mac, $pair, $ip, mb_substr((string)($in['local_ip'] ?? ''), 0, 45), $fw, $now, $now]);
}
reply(200, ['ok' => true, 'state' => 'pending']);
