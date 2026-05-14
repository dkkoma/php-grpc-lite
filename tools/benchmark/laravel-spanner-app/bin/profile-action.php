<?php

declare(strict_types=1);

use BenchApp\Bench\SpannerBench;
use Illuminate\Contracts\Console\Kernel as ConsoleKernel;

require __DIR__ . '/../vendor/autoload.php';

/** @var Illuminate\Foundation\Application $app */
$app = require __DIR__ . '/../bootstrap/app.php';
$app->make(ConsoleKernel::class)->bootstrap();

$action = $argv[1] ?? 'transaction_select2_update1_insert1';
$iterations = max(1, (int) ($argv[2] ?? '1'));

$bench = $app->make(SpannerBench::class);

$startedAt = hrtime(true);
for ($index = 0; $index < $iterations; $index++) {
    match ($action) {
        'select_1row_10col' => $bench->selectOneRow(),
        'dml_insert_10col' => $bench->dmlInsert(),
        'dml_update_10col' => $bench->dmlUpdate(),
        'dml_delete_10col' => $bench->dmlDelete(),
        'transaction_select2_update1_insert1' => $bench->mixedTransaction(),
        default => throw new InvalidArgumentException("unknown action: $action"),
    };
}
$elapsedMs = (hrtime(true) - $startedAt) / 1_000_000;

fwrite(STDOUT, json_encode([
    'ok' => true,
    'action' => $action,
    'iterations' => $iterations,
    'elapsed_ms' => $elapsedMs,
    'pid' => getmypid(),
], JSON_THROW_ON_ERROR) . PHP_EOL);
