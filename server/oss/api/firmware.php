<?php
// Prenos posodobitve za vrata. Vrata pošljejo svoj MAC in ključ v glavah
// X-Door-Mac / X-Door-Token in dobijo samo programsko opremo, ki jim je dodeljena.
require __DIR__ . '/../lib/bootstrap.php';

$id = (int)($_GET['id'] ?? 0);
$mac = strtoupper((string)($_SERVER['HTTP_X_DOOR_MAC'] ?? ''));
$token = (string)($_SERVER['HTTP_X_DOOR_TOKEN'] ?? '');

$door = $token !== '' ? q('SELECT id, mac, firmware_id FROM vr_doors WHERE token_hash = ?', [hash('sha256', $token)])->fetch() : null;
if (!$door || $door['mac'] !== $mac || (int)$door['firmware_id'] !== $id) {
    http_response_code(403);
    header('Content-Type: text/plain');
    exit('forbidden');
}
$fw = q('SELECT version, size, md5, data FROM vr_firmware WHERE id = ?', [$id])->fetch();
if (!$fw) {
    http_response_code(404);
    exit('not found');
}
log_event((int)$door['id'], null, 'ota', 'Vrata prenašajo programsko opremo ' . $fw['version'] . '.');
header('Content-Type: application/octet-stream');
header('Content-Length: ' . strlen($fw['data']));
header('Cache-Control: no-store');
header('X-Firmware-MD5: ' . $fw['md5']);
echo $fw['data'];
