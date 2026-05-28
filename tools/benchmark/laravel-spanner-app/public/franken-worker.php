<?php

declare(strict_types=1);

use Illuminate\Http\Request;

require __DIR__ . '/../vendor/autoload.php';

/** @var Illuminate\Foundation\Application $app */
$app = require __DIR__ . '/../bootstrap/app.php';

while (frankenphp_handle_request(static function () use ($app): void {
    $app->handleRequest(Request::capture());
})) {
    gc_collect_cycles();
}
