<?php
// Glava, meni in noga strani (Bootstrap 4, enak videz kot stran na vratih).
defined('VRATA') or exit;

function nav_items(): array
{
    return [
        ['index.php',      'Vrata',              'doors.view'],
        ['monitoring.php', 'Nadzor',             'monitoring.view'],
        ['log.php',        'Dnevnik',            'log.view'],
        ['groups.php',     'Skupine',            'groups.edit'],
        ['firmware.php',   'Programska oprema',  'firmware.manage'],
        ['users.php',      'Uporabniki',         'users.manage'],
        ['roles.php',      'Vloge',              'users.manage'],
    ];
}

function page_start(string $title, int $refreshSeconds = 0): void
{
    header('Content-Type: text/html; charset=utf-8');
    header('X-Frame-Options: DENY');
    header('X-Content-Type-Options: nosniff');
    header('Referrer-Policy: same-origin');
    $u = current_user();
    $self = basename($_SERVER['SCRIPT_NAME'] ?? '');
    echo '<!DOCTYPE html><html lang="sl"><head><meta charset="UTF-8">';
    echo '<meta name="viewport" content="width=device-width, initial-scale=1.0">';
    if ($refreshSeconds > 0) echo '<meta http-equiv="refresh" content="' . (int)$refreshSeconds . '">';
    echo '<title>' . h($title) . ' – Vrata</title>';
    echo '<link rel="stylesheet" href="assets/bootstrap.min.css"><link rel="stylesheet" href="assets/app.css"></head><body>';
    echo '<nav class="navbar navbar-expand-md navbar-dark bg-dark mb-4"><a class="navbar-brand" href="index.php">Vrata</a>';
    if ($u) {
        echo '<button class="navbar-toggler" type="button" onclick="document.getElementById(\'nav\').classList.toggle(\'show\')">'
           . '<span class="navbar-toggler-icon"></span></button><div class="collapse navbar-collapse" id="nav"><ul class="navbar-nav mr-auto">';
        foreach (nav_items() as [$href, $label, $perm]) {
            if (!can($perm)) continue;
            $active = $self === $href ? ' active' : '';
            echo '<li class="nav-item' . $active . '"><a class="nav-link" href="' . h($href) . '">' . h($label) . '</a></li>';
        }
        echo '</ul><ul class="navbar-nav"><li class="nav-item"><a class="nav-link" href="account.php">'
           . h($u['display_name'] ?: $u['username']) . ' <small>(' . h($u['role_name']) . ')</small></a></li>'
           . '<li class="nav-item"><a class="nav-link" href="logout.php">Odjava</a></li></ul></div>';
    }
    echo '</nav><div class="container-fluid px-md-4">';
    echo '<h1 class="h3 mb-3">' . h($title) . '</h1>';
    if (!empty($_SESSION['flash'])) {
        foreach ($_SESSION['flash'] as [$type, $msg]) {
            echo '<div class="alert alert-' . h($type) . '">' . h($msg) . '</div>';
        }
        $_SESSION['flash'] = [];
    }
}

function page_end(): void
{
    echo '</div><footer class="text-center text-muted small my-4">' . h(cfg('site_name', 'Vrata')) . '</footer></body></html>';
}

// Majhni pomočniki za obrazce.
function select_options(array $options, $selected): string
{
    $html = '';
    foreach ($options as $value => $label) {
        $sel = ((string)$value === (string)$selected) ? ' selected' : '';
        $html .= '<option value="' . h($value) . '"' . $sel . '>' . h($label) . '</option>';
    }
    return $html;
}

function disabled_unless(bool $allowed): string
{
    return $allowed ? '' : ' disabled';
}
