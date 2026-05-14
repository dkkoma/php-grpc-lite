<?php

declare(strict_types=1);

use Illuminate\Support\Facades\Facade;
use Illuminate\Support\ServiceProvider;

return [
    'name' => 'php-grpc-lite-laravel-spanner-bench',
    'env' => 'benchmark',
    'debug' => false,
    'url' => 'http://localhost',
    'timezone' => 'UTC',
    'locale' => 'en',
    'fallback_locale' => 'en',
    'faker_locale' => 'en_US',
    'key' => 'base64:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=',
    'cipher' => 'AES-256-CBC',
    'previous_keys' => [],
    'maintenance' => [
        'driver' => 'file',
        'store' => 'database',
    ],
    'providers' => ServiceProvider::defaultProviders()->toArray(),
    'aliases' => Facade::defaultAliases()->toArray(),
];
