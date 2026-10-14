<?php
// Prijava, seje, pravice (vloge) in omejitev na vrata/skupine.
defined('VRATA') or exit;

// Vse pravice, ki jih lahko dodelite vlogi. Administrator ima vedno vse.
const PERMS = [
    'doors.view'      => 'Ogled vrat in njihovega stanja',
    'status.edit'     => 'Status vrat (zakleni / odpri / urnik) in takojšnji odklep',
    'calendar.edit'   => 'Lokalni koledar posameznih vrat',
    'groups.edit'     => 'Skupine in njihovi koledarji',
    'portal.users'    => 'Uporabniki portala telovadnice (imena in PIN-i)',
    'portal.config'   => 'Nastavitve portala (vklop, ime omrežja, časi)',
    'timing.edit'     => 'Intervali preverjanja in prenosa koledarja',
    'monitoring.view' => 'Nadzor (zadnji stik, prenosi, napake, naprava)',
    'log.view'        => 'Dnevnik dogodkov',
    'device.edit'     => 'Ime, opis in skupina vrat; sprejem novih in brisanje vrat',
    'wifi.edit'       => 'Wi-Fi omrežja in nastavitveno omrežje naprave',
    'firmware.manage' => 'Posodobitve programske opreme',
    'users.manage'    => 'Uporabniki spletne strani in vloge',
];

function session_begin(): void
{
    if (session_status() === PHP_SESSION_ACTIVE) return;
    session_name('vrata_sid');
    session_set_cookie_params([
        'lifetime' => 0,
        'path'     => '/',
        'secure'   => (bool)cfg('secure_cookies', true),
        'httponly' => true,
        'samesite' => 'Lax',
    ]);
    session_start();
}

function current_user(): ?array
{
    static $user = false;
    if ($user !== false) return $user;
    session_begin();
    $user = null;
    $id = (int)($_SESSION['uid'] ?? 0);
    if ($id <= 0) return null;
    $u = q('SELECT u.*, r.name AS role_name, r.is_system FROM vr_users u JOIN vr_roles r ON r.id = u.role_id
            WHERE u.id = ? AND u.active = 1', [$id])->fetch();
    if (!$u) return null;
    // Seja poteče po 8 urah nedejavnosti.
    if (time() - (int)($_SESSION['seen'] ?? 0) > 8 * 3600) {
        session_destroy();
        return null;
    }
    $_SESSION['seen'] = time();
    $u['perms'] = $u['is_system']
        ? array_keys(PERMS)
        : q('SELECT perm FROM vr_role_permissions WHERE role_id = ?', [$u['role_id']])->fetchAll(PDO::FETCH_COLUMN);
    $u['group_ids'] = q('SELECT group_id FROM vr_user_groups WHERE user_id = ?', [$id])->fetchAll(PDO::FETCH_COLUMN);
    $u['door_ids'] = q('SELECT door_id FROM vr_user_doors WHERE user_id = ?', [$id])->fetchAll(PDO::FETCH_COLUMN);
    $user = $u;
    return $user;
}

function require_login(): array
{
    $u = current_user();
    if (!$u) {
        header('Location: login.php');
        exit;
    }
    return $u;
}

function can(string $perm): bool
{
    $u = current_user();
    return $u && in_array($perm, $u['perms'], true);
}

function deny(string $message = 'Za to stran nimate pravic.'): never
{
    http_response_code(403);
    page_start('Ni dostopa');
    echo '<div class="alert alert-danger">' . h($message) . '</div>';
    page_end();
    exit;
}

function require_perm(string $perm): void
{
    require_login();
    if (!can($perm)) deny();
}

// SQL pogoj za vrata, ki jih uporabnik sme videti (alias tabele vrat = $a).
function door_scope(string $a = 'd'): array
{
    $u = current_user();
    if (!$u) return ['0', []];
    if ($u['all_doors']) return ['1', []];
    $parts = [];
    $params = [];
    if ($u['group_ids']) {
        $parts[] = "$a.group_id IN (" . implode(',', array_fill(0, count($u['group_ids']), '?')) . ')';
        $params = array_merge($params, $u['group_ids']);
    }
    if ($u['door_ids']) {
        $parts[] = "$a.id IN (" . implode(',', array_fill(0, count($u['door_ids']), '?')) . ')';
        $params = array_merge($params, $u['door_ids']);
    }
    return [$parts ? '(' . implode(' OR ', $parts) . ')' : '0', $params];
}

function can_see_door(array $door): bool
{
    $u = current_user();
    if (!$u) return false;
    if ($u['all_doors']) return true;
    return in_array((int)$door['id'], array_map('intval', $u['door_ids']), true)
        || ($door['group_id'] && in_array((int)$door['group_id'], array_map('intval', $u['group_ids']), true));
}

function can_see_group(int $groupId): bool
{
    $u = current_user();
    return $u && ($u['all_doors'] || in_array($groupId, array_map('intval', $u['group_ids']), true));
}

// Vrata po id-ju, če jih uporabnik sme videti; sicer 404/403.
function load_door(int $id): array
{
    $door = q('SELECT * FROM vr_doors WHERE id = ?', [$id])->fetch();
    if (!$door) {
        http_response_code(404);
        deny('Vrata ne obstajajo.');
    }
    if (!can_see_door($door)) deny('Teh vrat ne smete videti.');
    return $door;
}

// ---------------------------------------------------------------- CSRF -----

function csrf_token(): string
{
    session_begin();
    if (empty($_SESSION['csrf'])) $_SESSION['csrf'] = bin2hex(random_bytes(16));
    return $_SESSION['csrf'];
}

function csrf_field(): string
{
    return '<input type="hidden" name="csrf" value="' . h(csrf_token()) . '">';
}

function csrf_check(): void
{
    if ($_SERVER['REQUEST_METHOD'] !== 'POST') return;
    if (!hash_equals(csrf_token(), (string)($_POST['csrf'] ?? ''))) {
        http_response_code(400);
        exit('Seja je potekla. Osvežite stran in poskusite znova.');
    }
}

// ------------------------------------------------------- login throttle -----

function login_blocked(string $username): bool
{
    $ip = client_ip();
    $byIp = (int)q('SELECT COUNT(*) FROM vr_login_attempts WHERE ip = ? AND success = 0
                    AND created_at > (UTC_TIMESTAMP() - INTERVAL 15 MINUTE)', [$ip])->fetchColumn();
    $byUser = (int)q('SELECT COUNT(*) FROM vr_login_attempts WHERE username = ? AND success = 0
                      AND created_at > (UTC_TIMESTAMP() - INTERVAL 15 MINUTE)', [$username])->fetchColumn();
    return $byIp >= 10 || $byUser >= 5;
}

function login_record(string $username, bool $success): void
{
    q('INSERT INTO vr_login_attempts (ip, username, success, created_at) VALUES (?, ?, ?, ?)',
      [client_ip(), mb_substr($username, 0, 64), $success ? 1 : 0, now_utc()]);
}

// ---------------------------------------------------------------- flash -----

function flash(string $type, string $message): void
{
    session_begin();
    $_SESSION['flash'][] = [$type, $message];
}

function redirect(string $to): never
{
    header('Location: ' . $to);
    exit;
}
