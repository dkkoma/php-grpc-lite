<?php

declare(strict_types=1);

use Illuminate\Http\Request;

require __DIR__ . '/../vendor/autoload.php';

/** @var Illuminate\Foundation\Application $app */
$app = require_once __DIR__ . '/../bootstrap/app.php';
$app->handleRequest(Request::capture());
