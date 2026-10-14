<?php
// Skupni začetek vseh strani: nastavitve, povezava z bazo, pomožne funkcije.
define('VRATA', true);

$cfgFile = __DIR__ . '/../config.php';
if (!is_file($cfgFile)) {
    http_response_code(500);
    exit('Manjka config.php. Kopirajte config.sample.php v config.php in vpišite podatke za bazo.');
}
$CONFIG = require $cfgFile;

date_default_timezone_set('Europe/Ljubljana');
mb_internal_encoding('UTF-8');

function cfg(string $key, $default = null)
{
    global $CONFIG;
    return $CONFIG[$key] ?? $default;
}

function db(): PDO
{
    static $pdo = null;
    if ($pdo === null) {
        $pdo = new PDO(cfg('db_dsn'), cfg('db_user'), cfg('db_pass'), [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES   => false,
        ]);
        $pdo->exec("SET time_zone = '+00:00'");  // vsi časi v bazi so UTC
        upgrade_schema($pdo);
    }
    return $pdo;
}

// Stolpci, dodani v novejših različicah, se v obstoječo bazo dodajo sami
// (ročno: glej "Nadgradnja" v README.md).
function upgrade_schema(PDO $pdo): void
{
    try {
        if (!$pdo->query("SHOW COLUMNS FROM vr_doors LIKE 'portal_pause_s'")->fetch()) {
            $pdo->exec('ALTER TABLE vr_doors ADD COLUMN portal_pause_s INT NOT NULL DEFAULT 540 AFTER portal_unlock_s');
        }
    } catch (Throwable $e) {
        // Tabel še ni (namestitev) ali uporabnik baze nima pravice ALTER.
    }
}

function q(string $sql, array $params = []): PDOStatement
{
    $st = db()->prepare($sql);
    $st->execute($params);
    return $st;
}

function now_utc(): string
{
    return gmdate('Y-m-d H:i:s');
}

function h($s): string
{
    return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function utc_ts(?string $utc): int
{
    if (!$utc) return 0;
    return (new DateTimeImmutable($utc, new DateTimeZone('UTC')))->getTimestamp();
}

function fmt_time(?string $utc, string $format = 'd.m.Y H:i:s'): string
{
    if (!$utc) return '–';
    return (new DateTimeImmutable($utc, new DateTimeZone('UTC')))
        ->setTimezone(new DateTimeZone('Europe/Ljubljana'))->format($format);
}

function fmt_ts(int $ts, string $format = 'd.m.Y H:i'): string
{
    if ($ts <= 0) return '–';
    return (new DateTimeImmutable('@' . $ts))->setTimezone(new DateTimeZone('Europe/Ljubljana'))->format($format);
}

function fmt_ago(?string $utc): string
{
    if (!$utc) return 'nikoli';
    $s = time() - utc_ts($utc);
    if ($s < 0) $s = 0;
    if ($s < 90) return 'pred ' . $s . ' s';
    if ($s < 5400) return 'pred ' . round($s / 60) . ' min';
    if ($s < 172800) return 'pred ' . round($s / 3600) . ' h';
    return 'pred ' . round($s / 86400) . ' dnevi';
}

function fmt_duration(int $s): string
{
    if ($s < 120) return $s . ' s';
    if ($s < 7200) return round($s / 60) . ' min';
    if ($s < 172800) return round($s / 3600, 1) . ' h';
    return round($s / 86400, 1) . ' dni';
}

function client_ip(): string
{
    return substr((string)($_SERVER['REMOTE_ADDR'] ?? ''), 0, 45);
}

// Dnevnik dogodkov (vrata in spletna stran).
function log_event(?int $doorId, ?int $userId, string $type, string $message, ?string $atUtc = null): void
{
    q('INSERT INTO vr_events (created_at, door_id, user_id, type, message) VALUES (?, ?, ?, ?, ?)',
      [$atUtc ?? now_utc(), $doorId, $userId, substr($type, 0, 24), mb_substr($message, 0, 500)]);
}

// Občasno počiščimo stare zapise, da baza ne raste v nedogled.
function housekeeping(): void
{
    if (random_int(1, 200) !== 1) return;
    q('DELETE FROM vr_events WHERE created_at < (UTC_TIMESTAMP() - INTERVAL 400 DAY)');
    q('DELETE FROM vr_login_attempts WHERE created_at < (UTC_TIMESTAMP() - INTERVAL 2 DAY)');
    q('DELETE FROM vr_pending WHERE last_seen_at < (UTC_TIMESTAMP() - INTERVAL 14 DAY)');
}
