<?php
// Pomožne funkcije za vrata: statusi, uporabniška imena, nastavitve za napravo.
defined('VRATA') or exit;

const STATUS_LABELS = [
    0 => 'Zaklenjeno (LOCK)',
    1 => 'Odprto (OPEN)',
    2 => 'Urnik skupine (SCHEDULE REMOTE)',
    3 => 'Lokalni urnik (SCHEDULE LOCAL)',
];

const STATUS_BADGES = [0 => 'danger', 1 => 'success', 2 => 'info', 3 => 'primary'];

const POLL_MIN = 30, POLL_MAX = 3600;
const CAL_MIN = 60, CAL_MAX = 3600;

function status_badge(int $status): string
{
    $label = STATUS_LABELS[$status] ?? ('Status ' . $status);
    return '<span class="badge badge-' . (STATUS_BADGES[$status] ?? 'secondary') . '">' . h($label) . '</span>';
}

function clamp_int($v, int $min, int $max): int
{
    $v = (int)$v;
    return max($min, min($max, $v));
}

// Uporabniško ime iz imena: male črke, brez presledkov, šumniki -> c/s/z ...
// MORA se ujemati s funkcijo normalizeUser() v programski opremi vrat (ical.cpp).
function normalize_username(string $s): string
{
    static $map = null;
    if ($map === null) {
        $map = [];
        $table = [
            'a' => 'ÀÁÂÃÄÅàáâãäå', 'c' => 'ÇçČčĆć', 'd' => 'Đđ', 'e' => 'ÈÉÊËèéêë',
            'i' => 'ÌÍÎÏìíîï', 'n' => 'Ññ', 'o' => 'ÒÓÔÕÖØòóôõöø', 's' => 'Ššß',
            'u' => 'ÙÚÛÜùúûü', 'y' => 'Ýýÿ', 'z' => 'Žž',
        ];
        foreach ($table as $to => $chars) {
            foreach (mb_str_split($chars) as $ch) $map[$ch] = $to;
        }
    }
    $out = '';
    foreach (mb_str_split($s) as $ch) {
        if (isset($map[$ch])) {
            $out .= $map[$ch];
        } elseif (strlen($ch) === 1 && ctype_alnum($ch)) {
            $out .= strtolower($ch);
        }
    }
    return substr($out, 0, 20);
}

function valid_pin(string $pin): bool
{
    return (bool)preg_match('/^[0-9]{4,8}$/', $pin);
}

function random_pin(): string
{
    return (string)random_int(100000, 999999);
}

// Vsaka sprememba, ki jo morajo vrata dobiti, poveča config_rev (za prikaz "čaka na prenos").
function bump_rev(array $doorIds): void
{
    $doorIds = array_values(array_filter(array_map('intval', $doorIds)));
    if (!$doorIds) return;
    $in = implode(',', array_fill(0, count($doorIds), '?'));
    q("UPDATE vr_doors SET config_rev = config_rev + 1 WHERE id IN ($in)", $doorIds);
}

function bump_rev_group(int $groupId): void
{
    q('UPDATE vr_doors SET config_rev = config_rev + 1 WHERE group_id = ?', [$groupId]);
}

function door_report(array $door): array
{
    $r = $door['report'] ? json_decode((string)$door['report'], true) : null;
    return is_array($r) ? $r : [];
}

// Vrata so "na povezavi", če so se oglasila v treh intervalih preverjanja.
function door_online(array $door): bool
{
    if (!$door['last_seen_at']) return false;
    $limit = max(3 * (int)$door['poll_s'], 120) + 30;
    return time() - utc_ts($door['last_seen_at']) <= $limit;
}

function online_badge(array $door): string
{
    if (!$door['mac']) return '<span class="badge badge-secondary">ni naprave</span>';
    if ($door['token_plain']) return '<span class="badge badge-warning">čaka na prevzem ključa</span>';
    return door_online($door)
        ? '<span class="badge badge-success">povezana</span>'
        : '<span class="badge badge-danger">brez povezave</span> <small class="text-muted">' . h(fmt_ago($door['last_seen_at'])) . '</small>';
}

function door_state_html(array $door): string
{
    $r = door_report($door);
    if (!$r) return '<span class="text-muted">–</span>';
    $open = !empty($r['door_open']);
    $html = $open ? '<span class="badge badge-success">ODPRTA</span>' : '<span class="badge badge-secondary">ZAKLENJENA</span>';
    if ($open && !empty($r['open_reason'])) $html .= ' <small>' . h($r['open_reason']) . '</small>';
    if (!door_online($door)) $html .= ' <small class="text-muted">(zadnje znano)</small>';
    return $html;
}

// Nastavitve, ki jih vrata dobijo ob vsakem preverjanju.
function door_config(array $door): array
{
    $group = $door['group_id'] ? q('SELECT * FROM vr_groups WHERE id = ?', [$door['group_id']])->fetch() : null;
    $users = q('SELECT username, pin FROM vr_portal_users WHERE door_id = ? AND active = 1 ORDER BY username',
               [$door['id']])->fetchAll();
    $cfg = [
        'rev'         => (int)$door['config_rev'],
        'name'        => $door['name'],
        'status'      => (int)$door['status'],
        'remote_ical' => ($group && !$group['disabled']) ? $group['ical_url'] : '',
        'local_ical'  => $door['local_ical_url'],
        'poll_s'      => clamp_int($door['poll_s'], POLL_MIN, POLL_MAX),
        'calendar_s'  => clamp_int($door['calendar_s'], CAL_MIN, CAL_MAX),
        'setup_ap'    => $door['setup_ap'],
        'open_until'  => utc_ts($door['open_until']),
        'portal'      => [
            'enabled'    => (bool)$door['portal_enabled'],
            'ssid'       => $door['portal_ssid'],
            'unlock_s'   => (int)$door['portal_unlock_s'],
            'pause_s'    => (int)($door['portal_pause_s'] ?? max(0, (int)$door['portal_unlock_s'] - 60)),
            'before_min' => (int)$door['pin_before_min'],
            'after_min'  => (int)$door['pin_after_min'],
            'users'      => array_map(fn($u) => $u['username'] . ':' . $u['pin'], $users),
        ],
    ];
    if ((int)$door['wifi_rev'] > 0) {
        $cfg['wifi_rev'] = (int)$door['wifi_rev'];
        $cfg['wifi'] = json_decode((string)$door['wifi_json'], true) ?: [];
    }
    if ($door['firmware_id']) {
        $fw = q('SELECT id, version, md5, size FROM vr_firmware WHERE id = ?', [$door['firmware_id']])->fetch();
        if ($fw) {
            $cfg['firmware'] = [
                'version' => $fw['version'],
                'md5'     => $fw['md5'],
                'size'    => (int)$fw['size'],
                'url'     => rtrim((string)cfg('base_url'), '/') . '/api/firmware.php?id=' . (int)$fw['id'],
            ];
        }
    }
    return $cfg;
}

function groups_all(): array
{
    return q('SELECT * FROM vr_groups ORDER BY name')->fetchAll();
}
