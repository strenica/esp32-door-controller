<?php
// Kopirajte to datoteko v config.php in vpišite podatke za bazo.
// config.php je PHP, zato se njena vsebina nikoli ne prikaže v brskalniku.
return [
    // phpMyAdmin pokaže ime baze levo zgoraj; uporabnik in geslo sta tista,
    // s katerima se PHP povezuje na MySQL (ne vaše geslo za phpMyAdmin, če je drugačno).
    'db_dsn'  => 'mysql:host=localhost;dbname=IME_BAZE;charset=utf8mb4',
    'db_user' => 'UPORABNIK',
    'db_pass' => 'GESLO',

    // Javni naslov te strani brez poševnice na koncu (vrata od tu prenašajo posodobitve).
    // Zamenjajte s svojo domeno; vrata zavrnejo strežnik brez veljavnega potrdila.
    'base_url' => 'https://example.com/oss',

    // Besedilo v nogi strani (npr. "Vrata · Moja šola").
    'site_name' => 'Vrata',

    // true = piškotek seje samo prek HTTPS (pustite true, če stran teče na https).
    'secure_cookies' => true,
];
