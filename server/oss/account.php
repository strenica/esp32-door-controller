<?php
require __DIR__ . '/lib/web.php';
$me = require_login();

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $old = (string)($_POST['old'] ?? '');
    $new = (string)($_POST['new'] ?? '');
    $row = q('SELECT password_hash FROM vr_users WHERE id = ?', [$me['id']])->fetch();
    if (!password_verify($old, $row['password_hash'])) {
        flash('danger', 'Staro geslo ni pravilno.');
    } elseif (mb_strlen($new) < 10) {
        flash('danger', 'Novo geslo mora imeti vsaj 10 znakov.');
    } elseif ($new !== (string)($_POST['new2'] ?? '')) {
        flash('danger', 'Novi gesli se ne ujemata.');
    } else {
        q('UPDATE vr_users SET password_hash = ? WHERE id = ?', [password_hash($new, PASSWORD_DEFAULT), $me['id']]);
        log_event(null, (int)$me['id'], 'user', 'Uporabnik ' . $me['username'] . ' je spremenil svoje geslo.');
        flash('success', 'Geslo je spremenjeno.');
    }
    redirect('account.php');
}

page_start('Moj račun');
echo '<p>Uporabniško ime: <b>' . h($me['username']) . '</b>, vloga: <b>' . h($me['role_name']) . '</b>.</p>';
echo '<form method="post" class="card card-body" style="max-width:420px">' . csrf_field()
   . '<h5>Sprememba gesla</h5>'
   . '<div class="form-group"><label>Staro geslo</label><input class="form-control" type="password" name="old" required></div>'
   . '<div class="form-group"><label>Novo geslo (vsaj 10 znakov)</label><input class="form-control" type="password" name="new" required></div>'
   . '<div class="form-group"><label>Ponovite novo geslo</label><input class="form-control" type="password" name="new2" required></div>'
   . '<button class="btn btn-primary">Spremeni geslo</button></form>';
page_end();
