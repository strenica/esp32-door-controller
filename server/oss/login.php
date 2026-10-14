<?php
require __DIR__ . '/lib/web.php';

if (current_user()) redirect('index.php');
if ((int)q('SELECT COUNT(*) FROM vr_users')->fetchColumn() === 0) redirect('install.php');

$error = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $username = trim((string)($_POST['username'] ?? ''));
    $password = (string)($_POST['password'] ?? '');
    if (login_blocked($username)) {
        $error = 'Preveč neuspešnih poskusov. Poskusite znova čez 15 minut.';
    } else {
        $u = q('SELECT * FROM vr_users WHERE username = ? AND active = 1', [$username])->fetch();
        if ($u && password_verify($password, $u['password_hash'])) {
            login_record($username, true);
            session_regenerate_id(true);
            $_SESSION['uid'] = (int)$u['id'];
            $_SESSION['seen'] = time();
            if (password_needs_rehash($u['password_hash'], PASSWORD_DEFAULT)) {
                q('UPDATE vr_users SET password_hash = ? WHERE id = ?', [password_hash($password, PASSWORD_DEFAULT), $u['id']]);
            }
            q('UPDATE vr_users SET last_login_at = ? WHERE id = ?', [now_utc(), $u['id']]);
            redirect('index.php');
        }
        login_record($username, false);
        sleep(1);
        $error = 'Napačno uporabniško ime ali geslo.';
    }
}

page_start('Prijava');
if ($error) echo '<div class="alert alert-danger">' . h($error) . '</div>';
echo '<form method="post" class="card card-body" style="max-width:380px">' . csrf_field()
   . '<div class="form-group"><label for="u">Uporabniško ime</label>'
   . '<input class="form-control" id="u" name="username" autocomplete="username" required autofocus></div>'
   . '<div class="form-group"><label for="p">Geslo</label>'
   . '<input class="form-control" id="p" type="password" name="password" autocomplete="current-password" required></div>'
   . '<button class="btn btn-primary">Prijava</button></form>';
page_end();
