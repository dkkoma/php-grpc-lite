<?php
declare(strict_types=1);

require __DIR__ . '/BenchTelemetry.php';

use Google\Cloud\Spanner\Database;
use Google\Cloud\Spanner\SpannerClient;
use Google\Cloud\Spanner\Transaction;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\DatabaseAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerEnv;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;

$args = $argv;
array_shift($args);

$suite = 'spanner-real-client';
$implementation = 'php-grpc-lite';
$autoload = 'vendor/autoload.php';
$target = getenv('SPANNER_EMULATOR_HOST') ?: 'spanner-emulator:9010';
$warmupCalls = 5;
$calls = 100;
$transport = 'native';

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--suite') {
        $suite = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--suite=')) {
        $suite = substr($arg, strlen('--suite='));
    } elseif ($arg === '--implementation') {
        $implementation = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--implementation=')) {
        $implementation = substr($arg, strlen('--implementation='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $autoload === '' || $target === '') {
    usage('suite, implementation, autoload, and target are required');
}
if ($warmupCalls < 0 || $calls <= 0) {
    usage('warmup-calls and calls must be valid');
}
if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}

require $autoload;

putenv('SPANNER_EMULATOR_HOST=' . $target);
$_SERVER['SPANNER_EMULATOR_HOST'] = $target;

$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$suffix = substr(bin2hex(random_bytes(4)), 0, 8);
$implementationPart = preg_replace('/[^a-z0-9]+/', '', strtolower($implementation)) ?: 'impl';
$instanceId = 'pglr-' . substr($implementationPart, 0, 12) . '-' . $suffix;
$databaseId = 'benchdb';
$instanceName = null;

$instanceAdmin = new InstanceAdminGrpcClient($target, SpannerEnv::options());
$databaseAdmin = new DatabaseAdminGrpcClient($target, SpannerEnv::options());

try {
    $instanceName = SpannerEnv::createInstance($instanceAdmin, $instanceId);
    SpannerEnv::createDatabase($databaseAdmin, $instanceName, $databaseId, [benchTableDdl()]);

    $spanner = new SpannerClient([
        'projectId' => 'test-project',
        'transport' => 'grpc',
        'apiEndpoint' => $target,
    ]);
    $database = $spanner->connect($instanceId, $databaseId);

    seedSelectRow($database);
    warmup($database, $warmupCalls);

    measureSelect($benchTelemetry, $database, $calls, $target, $transport, $warmupCalls);
    measureDml($benchTelemetry, $database, 'dml_insert_10col', $calls, $target, $transport, $warmupCalls);
    measureDml($benchTelemetry, $database, 'dml_update_10col', $calls, $target, $transport, $warmupCalls);
    measureDml($benchTelemetry, $database, 'dml_delete_10col', $calls, $target, $transport, $warmupCalls);
} finally {
    if ($instanceName !== null) {
        SpannerEnv::deleteInstance($instanceAdmin, $instanceId);
    }
}

echo "OTEL spans exported.\n";

function benchTableDdl(): string
{
    return <<<'SQL'
CREATE TABLE BenchRows (
  Id INT64 NOT NULL,
  DateA DATE,
  DateB DATE,
  StringA STRING(MAX),
  StringB STRING(MAX),
  IntA INT64,
  IntB INT64,
  BoolA BOOL,
  FloatA FLOAT64,
  StringC STRING(MAX)
) PRIMARY KEY (Id)
SQL;
}

function seedSelectRow(Database $database): void
{
    runDmlTransaction($database, insertSql(1));
}

function warmup(Database $database, int $warmupCalls): void
{
    for ($index = 0; $index < $warmupCalls; $index++) {
        drainSelect($database);
        $id = 10_000_000 + $index;
        runDmlTransaction($database, insertSql($id));
        runDmlTransaction($database, updateSql($id));
        runDmlTransaction($database, deleteSql($id));
    }
}

function measureSelect(
    BenchTelemetry $benchTelemetry,
    Database $database,
    int $calls,
    string $target,
    string $transport,
    int $warmupCalls,
): void {
    $benchTelemetry->setContext('small_select_1row_10col', commonContext($target, $calls, $transport, $warmupCalls) + [
        'benchmark.spanner_api' => 'Database::execute',
        'benchmark.operation_shape' => 'select_1row_10col',
        'benchmark.rows' => 1,
        'benchmark.columns' => 10,
    ]);

    for ($index = 0; $index < $calls; $index++) {
        $startNs = hrtime(true);
        $statusCode = 1;
        try {
            drainSelect($database);
        } catch (Throwable $throwable) {
            $statusCode = 2;
            throw $throwable;
        } finally {
            $endNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('Spanner.Database.execute', $startNs, $endNs, [
                'rpc.service' => 'google.spanner.v1.Spanner',
                'rpc.method' => 'ExecuteStreamingSql',
            ], $statusCode);
        }
    }
}

function measureDml(
    BenchTelemetry $benchTelemetry,
    Database $database,
    string $measurement,
    int $calls,
    string $target,
    string $transport,
    int $warmupCalls,
): void {
    $benchTelemetry->setContext($measurement, commonContext($target, $calls, $transport, $warmupCalls) + [
        'benchmark.spanner_api' => 'Transaction::executeUpdate',
        'benchmark.operation_shape' => $measurement,
        'benchmark.rows' => 1,
        'benchmark.columns' => 10,
    ]);

    for ($index = 0; $index < $calls; $index++) {
        $id = operationId($measurement, $index);
        if ($measurement !== 'dml_insert_10col') {
            runDmlTransaction($database, insertSql($id));
        }

        $startNs = hrtime(true);
        $statusCode = 1;
        try {
            $rowCount = match ($measurement) {
                'dml_insert_10col' => runDmlTransaction($database, insertSql($id)),
                'dml_update_10col' => runDmlTransaction($database, updateSql($id)),
                'dml_delete_10col' => runDmlTransaction($database, deleteSql($id)),
                default => throw new LogicException("unknown DML measurement: $measurement"),
            };
            if ($rowCount !== 1) {
                throw new RuntimeException("expected 1 updated row, got $rowCount");
            }
        } catch (Throwable $throwable) {
            $statusCode = 2;
            throw $throwable;
        } finally {
            $endNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('Spanner.Transaction.executeUpdate', $startNs, $endNs, [
                'rpc.service' => 'google.spanner.v1.Spanner',
                'rpc.method' => 'ExecuteStreamingSql+Commit',
            ], $statusCode);
        }

        if ($measurement === 'dml_insert_10col') {
            runDmlTransaction($database, deleteSql($id));
        }
    }
}

/** @return array<string, int|string> */
function commonContext(string $target, int $calls, string $transport, int $warmupCalls): array
{
    return [
        'benchmark.target' => $target,
        'benchmark.calls' => $calls,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.transport' => $transport,
        'benchmark.spanner_path' => 'google-cloud-spanner',
    ];
}

function drainSelect(Database $database): void
{
    $result = $database->execute(selectSql());
    $rows = 0;
    foreach ($result->rows() as $_row) {
        $rows++;
    }
    if ($rows !== 1) {
        throw new RuntimeException("expected 1 selected row, got $rows");
    }
}

function runDmlTransaction(Database $database, string $sql): int
{
    return $database->runTransaction(static function (Transaction $transaction) use ($sql): int {
        $rowCount = $transaction->executeUpdate($sql);
        $transaction->commit();
        return $rowCount;
    });
}

function operationId(string $measurement, int $index): int
{
    return match ($measurement) {
        'dml_insert_10col' => 100_000 + $index,
        'dml_update_10col' => 200_000 + $index,
        'dml_delete_10col' => 300_000 + $index,
        default => 900_000 + $index,
    };
}

function selectSql(): string
{
    return 'SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM BenchRows WHERE Id = 1';
}

function insertSql(int $id): string
{
    return "INSERT INTO BenchRows (Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC) VALUES ($id, DATE '2026-01-01', DATE '2026-01-02', 'alpha', 'beta', 123, 456, TRUE, 1.25, 'gamma')";
}

function updateSql(int $id): string
{
    return "UPDATE BenchRows SET StringA = 'updated', IntA = IntA + 1 WHERE Id = $id";
}

function deleteSql(int $id): string
{
    return "DELETE FROM BenchRows WHERE Id = $id";
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/spanner-real-client.php --suite=spanner-real-client --implementation=php-grpc-lite [--calls=100] [--warmup-calls=5] [--target=spanner-emulator:9010]\n");
    exit(2);
}
