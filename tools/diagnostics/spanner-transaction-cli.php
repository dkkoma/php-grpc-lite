<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

use Google\Cloud\Spanner\SpannerClient;
use Google\Cloud\Spanner\Transaction;

$iterations = (int) ($argv[1] ?? getenv('SPANNER_TRANSACTION_DIAGNOSTIC_ITERATIONS') ?: 200);
if ($iterations <= 0) {
    fwrite(STDERR, "Usage: php tools/diagnostics/spanner-transaction-cli.php [iterations]\n");
    exit(2);
}

$projectId = getenv('GOOGLE_CLOUD_PROJECT') ?: getenv('DB_SPANNER_PROJECT_ID') ?: getenv('LARAVEL_SPANNER_PROJECT_ID') ?: '';
$instanceId = getenv('DB_SPANNER_INSTANCE') ?: getenv('DB_SPANNER_INSTANCE_ID') ?: getenv('LARAVEL_SPANNER_INSTANCE_ID') ?: '';
$databaseId = getenv('DB_SPANNER_DATABASE') ?: getenv('DB_SPANNER_DATABASE_ID') ?: getenv('LARAVEL_SPANNER_DATABASE_ID') ?: '';

if ($projectId === '' || $instanceId === '' || $databaseId === '') {
    fwrite(STDERR, "GOOGLE_CLOUD_PROJECT/DB_SPANNER_PROJECT_ID, DB_SPANNER_INSTANCE(_ID), and DB_SPANNER_DATABASE(_ID) are required.\n");
    exit(2);
}

$spanner = new SpannerClient(['projectId' => $projectId]);
$database = $spanner->instance($instanceId)->database($databaseId);

// Warm the client, channel, and first Spanner session before measurement.
foreach ($database->execute('SELECT 1')->rows() as $_row) {
    break;
}

fwrite(STDERR, "Diagnostic fixture: measures full Spanner transaction wall time, not Commit RPC alone. Use GRPC_LITE_TRACE_FILE to inspect per-RPC rpc.end and wire frames.\n");

$times = [];
for ($index = 0; $index < $iterations; $index++) {
    $started = hrtime(true);
    $database->runTransaction(static function (Transaction $transaction) use ($index): void {
        foreach ($transaction->execute('SELECT @i', ['parameters' => ['i' => $index]])->rows() as $_row) {
            break;
        }
        $transaction->commit();
    });
    $times[] = (hrtime(true) - $started) / 1000.0;
}

sort($times);
$count = count($times);
$mean = array_sum($times) / $count;
$percentile = static function (float $quantile) use ($times, $count): float {
    $index = (int) floor(($count - 1) * $quantile);
    return $times[$index];
};

printf(
    "n=%d mean=%.0fus p50=%.0fus p90=%.0fus p99=%.0fus min=%.0fus max=%.0fus\n",
    $count,
    $mean,
    $percentile(0.50),
    $percentile(0.90),
    $percentile(0.99),
    $times[0],
    $times[$count - 1]
);
