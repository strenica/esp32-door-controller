<?php
// Vključi vse, kar potrebuje spletna stran (ne API za vrata).
require __DIR__ . '/bootstrap.php';
require __DIR__ . '/doors.php';
require __DIR__ . '/auth.php';
require __DIR__ . '/layout.php';

session_begin();
csrf_check();
