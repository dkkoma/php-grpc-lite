<?php

declare(strict_types=1);

use BenchApp\Bench\SpannerBench;
use Illuminate\Contracts\Console\Kernel as ConsoleKernel;

require __DIR__ . '/../vendor/autoload.php';

/** @var Illuminate\Foundation\Application $app */
$app = require __DIR__ . '/../bootstrap/app.php';
$app->make(ConsoleKernel::class)->bootstrap();

$bench = $app->make(SpannerBench::class);
$setup = $bench->setupDatabase();
$warmup = $bench->warmupSessionPool();

fwrite(STDOUT, json_encode([
    'ok' => true,
    'setup' => $setup,
    'warmup' => $warmup,
], JSON_THROW_ON_ERROR) . PHP_EOL);
