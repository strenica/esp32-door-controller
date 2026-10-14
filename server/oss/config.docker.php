<?php
// Konfiguracija za lokalni test s compose.yaml (docker compose up).
// Uporaba:  cp server/oss/config.docker.php server/oss/config.php
// config.php je v .gitignore, zato se ta datoteka ohrani kot predloga.
// Vrednosti so namerno javne in veljajo samo znotraj compose omrežja.
return [
    // host=db = ime storitve iz compose.yaml. Za lokalni PHP brez kontejnerja
    // bi bilo host=127.0.0.1.
    'db_dsn'  => 'mysql:host=db;dbname=vrata;charset=utf8mb4',
    'db_user' => 'vrata',
    'db_pass' => 'vrata',

    // Mora ustrezati naslovu, na katerem teče lokalni strežnik (brez /oss, ker
    // vgrajeni PHP strežnik streže kar iz mape oss).
    'base_url' => 'http://localhost:8000',

    // NUJNO za lokalni test: na http bi s true piškotek zavrnil in prijava bi se ponavljala.
    'secure_cookies' => false,

    'site_name' => 'Vrata (lokalni test)',
];
