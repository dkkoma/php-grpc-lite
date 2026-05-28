<?php

declare(strict_types=1);

use Illuminate\Http\Request;

require __DIR__ . '/../vendor/autoload.php';

/** @var Illuminate\Foundation\Application $app */
$app = require_once __DIR__ . '/../bootstrap/app.php';

$handleRequest = static function () use ($app): void {
    $app->handleRequest(Request::capture());
};

if (function_exists('frankenphp_handle_request')) {
    while (frankenphp_handle_request($handleRequest)) {
        gc_collect_cycles();
    }
    return;
}

$handleRequest();
