<?php
require __DIR__ . '/lib/web.php';

$_SESSION = [];
session_destroy();
redirect('login.php');
