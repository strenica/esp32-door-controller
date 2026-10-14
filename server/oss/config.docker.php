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

    // Mora ustrezati naslovu, na katerega se povezujejo VRATA, ne samo vi.
    // Za brskalnik na istem računalniku je localhost pravilen, a prava naprava
    // localhost razume kot sebe. Za preizkus s pravimi vrati vpišite IP tega
    // računalnika v omrežju, npr. 'http://192.168.1.10:8000' - vrata s tega
    // naslova prenašajo tudi posodobitve programske opreme.
    'base_url' => 'http://localhost:8000',

    // NUJNO za lokalni test: na http bi s true piškotek zavrnil in prijava bi se ponavljala.
    'secure_cookies' => false,

    'site_name' => 'Vrata (lokalni test)',
];
