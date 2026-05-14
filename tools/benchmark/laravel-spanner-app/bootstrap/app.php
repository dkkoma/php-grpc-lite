<?php

declare(strict_types=1);

use Colopl\Spanner\SpannerServiceProvider;
use Illuminate\Foundation\Application;
use Illuminate\Foundation\Configuration\Exceptions;
use Illuminate\Foundation\Configuration\Middleware;

return Application::configure(basePath: dirname(__DIR__))
    ->withRouting(api: __DIR__ . '/../routes/api.php', apiPrefix: '')
    ->withMiddleware(static function (Middleware $middleware): void {
        $middleware->web(remove: [
            \Illuminate\Foundation\Http\Middleware\ValidateCsrfToken::class,
        ]);
    })
    ->withExceptions(static function (Exceptions $exceptions): void {
        // Keep the fixture app minimal; Laravel's default exception handling is enough.
    })
    ->withProviders([
        SpannerServiceProvider::class,
    ])
    ->create();
