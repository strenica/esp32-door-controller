<?php
// Enkratna namestitev: ustvari prvega administratorja. Ko obstaja vsaj en uporabnik,
// ta stran ne naredi ničesar več (lahko jo tudi izbrišete).
require __DIR__ . '/lib/web.php';

$problem = '';
try {
    $hasTables = (bool)q("SHOW TABLES LIKE 'vr_users'")->fetch();
} catch (Throwable $e) {
    $hasTables = false;
    $problem = 'Povezava z bazo ni uspela: ' . $e->getMessage();
}
if (!$problem && !$hasTables) $problem = 'Tabel še ni. V phpMyAdmin uvozite datoteko install.sql.';
$userCount = $hasTables ? (int)q('SELECT COUNT(*) FROM vr_users')->fetchColumn() : 0;

$error = '';
if (!$problem && $userCount === 0 && $_SERVER['REQUEST_METHOD'] === 'POST') {
    $username = trim((string)($_POST['username'] ?? ''));
    $password = (string)($_POST['password'] ?? '');
    if (!preg_match('/^[A-Za-z0-9._-]{3,64}$/', $username)) {
        $error = 'Uporabniško ime: 3–64 znakov (črke, številke, . _ -).';
    } elseif (mb_strlen($password) < 8) {
        $error = 'Geslo mora imeti vsaj 8 znakov.';
    } elseif ($password !== (string)($_POST['password2'] ?? '')) {
        $error = 'Gesli se ne ujemata.';
    } else {
        q('INSERT INTO vr_users (username, display_name, password_hash, role_id, all_doors, active, created_at)
           VALUES (?, ?, ?, 1, 1, 1, ?)', [$username, 'Administrator', password_hash($password, PASSWORD_DEFAULT), now_utc()]);
        log_event(null, null, 'user', "Namestitev: ustvarjen administrator $username.");
        flash('success', 'Administrator je ustvarjen. Prijavite se. Datoteko install.php lahko zdaj izbrišete.');
        redirect('login.php');
    }
}

page_start('Namestitev');
if ($problem) {
    echo '<div class="alert alert-danger">' . h($problem) . '</div>';
} elseif ($userCount > 0) {
    echo '<div class="alert alert-info">Namestitev je že opravljena. <a href="login.php">Prijava</a></div>';
} else {
    if ($error) echo '<div class="alert alert-danger">' . h($error) . '</div>';
    echo '<p>Ustvarite prvega administratorja. Ta ima vse pravice.</p>';
    echo '<form method="post" class="card card-body" style="max-width:420px">' . csrf_field()
       . '<div class="form-group"><label>Uporabniško ime</label><input class="form-control" name="username" required></div>'
       . '<div class="form-group"><label>Geslo (vsaj 8 znakov)</label><input class="form-control" type="password" name="password" required></div>'
       . '<div class="form-group"><label>Ponovite geslo</label><input class="form-control" type="password" name="password2" required></div>'
       . '<button class="btn btn-primary">Ustvari</button></form>';
}
page_end();
