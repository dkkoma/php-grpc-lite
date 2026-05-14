<?php
declare(strict_types=1);

use Colopl\Spanner\Connection;
use Google\Cloud\Spanner\SpannerClient;
use Illuminate\Config\Repository as ConfigRepository;
use Illuminate\Database\DatabaseServiceProvider;
use Illuminate\Events\EventServiceProvider;
use Illuminate\Filesystem\Filesystem;
use Illuminate\Foundation\Application;
use Colopl\Spanner\SpannerServiceProvider;

const BENCH_TABLE = 'BenchRows';

$root = dirname(__DIR__, 2);
$appBase = __DIR__ . '/laravel-spanner-app';
$autoload = $appBase . '/vendor/autoload.php';

if (!is_file($autoload)) {
    http_response_code(500);
    echo "Laravel benchmark dependencies are missing. Run composer install in tools/benchmark/laravel-spanner-app.\n";
    return;
}

require $autoload;

header('content-type: application/json');

try {
    $action = $_SERVER['BENCH_ACTION'] ?? 'select_1row_10col';
    $connection = bootConnection($appBase);

    $result = match ($action) {
        'setup' => setupDatabase($connection),
        'warmup' => warmup($connection),
        'select_1row_10col' => selectOneRow($connection),
        'dml_insert_10col' => dmlInsert($connection),
        'dml_update_10col' => dmlUpdate($connection),
        'dml_delete_10col' => dmlDelete($connection),
        default => throw new InvalidArgumentException("unknown BENCH_ACTION: $action"),
    };

    echo json_encode(['ok' => true, 'pid' => getmypid(), 'action' => $action] + $result, JSON_THROW_ON_ERROR);
} catch (Throwable $throwable) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'pid' => getmypid(),
        'error' => $throwable::class,
        'message' => $throwable->getMessage(),
    ], JSON_THROW_ON_ERROR);
}

function bootConnection(string $appBase): Connection
{
    $target = $_SERVER['SPANNER_EMULATOR_HOST'] ?? getenv('SPANNER_EMULATOR_HOST') ?: 'spanner-emulator:9010';
    putenv('SPANNER_EMULATOR_HOST=' . $target);
    $_SERVER['SPANNER_EMULATOR_HOST'] = $target;

    $instanceId = $_SERVER['DB_SPANNER_INSTANCE_ID'] ?? getenv('DB_SPANNER_INSTANCE_ID') ?: 'laravel-bench-instance';
    $databaseId = $_SERVER['DB_SPANNER_DATABASE_ID'] ?? getenv('DB_SPANNER_DATABASE_ID') ?: 'laravel-bench-db';
    $projectId = $_SERVER['DB_SPANNER_PROJECT_ID'] ?? getenv('DB_SPANNER_PROJECT_ID') ?: 'test-project';

    $app = new Application($appBase);
    $app->useStoragePath($appBase . '/storage');
    $app->instance('files', new Filesystem());
    $app->instance('config', new ConfigRepository([
        'app' => [
            'name' => 'php-grpc-lite-laravel-spanner-bench',
            'env' => 'benchmark',
            'debug' => false,
        ],
        'database' => [
            'default' => 'spanner',
            'connections' => [
                'spanner' => [
                    'driver' => 'spanner',
                    'instance' => $instanceId,
                    'database' => $databaseId,
                    'cache_path' => $appBase . '/storage/framework/spanner',
                    'client' => [
                        'projectId' => $projectId,
                        'apiEndpoint' => $target,
                        'transport' => 'grpc',
                        'requestTimeout' => 600,
                    ],
                    'session_pool' => [
                        'minSessions' => 1,
                        'maxSessions' => 100,
                    ],
                ],
            ],
        ],
    ]));

    $app->register(EventServiceProvider::class);
    $app->register(DatabaseServiceProvider::class);
    $app->register(SpannerServiceProvider::class);
    $app->boot();

    $connection = $app->make('db')->connection('spanner');
    if (!$connection instanceof Connection) {
        throw new RuntimeException('spanner connection was not created');
    }
    return $connection;
}

function setupDatabase(Connection $connection): array
{
    ensureInstance($connection);

    if (!$connection->databaseExists()) {
        $connection->createDatabase([benchTableDdl()]);
    } else {
        try {
            $connection->runDdlBatch([benchTableDdl()]);
        } catch (Throwable) {
            // The table already exists. The emulator returns an error for duplicate DDL.
        }
    }

    $connection->transaction(static function (Connection $connection): void {
        $connection->statement('DELETE FROM ' . BENCH_TABLE . ' WHERE Id >= 1');
        $connection->statement(insertSql(1));
    });

    return ['setup' => true];
}

function warmup(Connection $connection): array
{
    $connection->warmupSessionPool();
    selectOneRow($connection);
    return ['warmup' => true];
}

function selectOneRow(Connection $connection): array
{
    $rows = $connection->transaction(static function (Connection $connection): array {
        return $connection->select('SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM ' . BENCH_TABLE . ' WHERE Id = ?', [1]);
    });
    if (count($rows) !== 1) {
        throw new RuntimeException('expected 1 row, got ' . count($rows));
    }
    return ['rows' => count($rows)];
}

function dmlInsert(Connection $connection): array
{
    $id = randomOperationId();
    $affected = $connection->transaction(static fn(Connection $connection): int => $connection->affectingStatement(insertSql($id)));
    if ($affected !== 1) {
        throw new RuntimeException("expected 1 affected row, got $affected");
    }
    return ['affected' => $affected];
}

function dmlUpdate(Connection $connection): array
{
    $id = randomOperationId();
    $connection->transaction(static fn(Connection $connection): bool => $connection->statement(insertSql($id)));
    $affected = $connection->transaction(static fn(Connection $connection): int => $connection->affectingStatement(updateSql($id)));
    if ($affected !== 1) {
        throw new RuntimeException("expected 1 affected row, got $affected");
    }
    return ['affected' => $affected];
}

function dmlDelete(Connection $connection): array
{
    $id = randomOperationId();
    $connection->transaction(static fn(Connection $connection): bool => $connection->statement(insertSql($id)));
    $affected = $connection->transaction(static fn(Connection $connection): int => $connection->affectingStatement(deleteSql($id)));
    if ($affected !== 1) {
        throw new RuntimeException("expected 1 affected row, got $affected");
    }
    return ['affected' => $affected];
}

function ensureInstance(Connection $connection): void
{
    $config = $connection->getConfig('client');
    assert(is_array($config));
    $spanner = new SpannerClient($config);
    $instanceId = (string) $connection->getConfig('instance');
    if ($spanner->instance($instanceId)->exists()) {
        return;
    }
    $operation = $spanner->createInstance($spanner->instanceConfiguration('emulator-config'), $instanceId);
    $operation->pollUntilComplete(['pollingIntervalSeconds' => 0.001]);
    if ($operation->error() !== null) {
        throw new RuntimeException(json_encode($operation->error(), JSON_THROW_ON_ERROR));
    }
}

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

function insertSql(int $id): string
{
    return sprintf(
        "INSERT INTO %s (Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC) VALUES (%d, DATE '2026-05-14', DATE '2026-05-15', 'alpha', 'beta', 123, 456, TRUE, 1.25, 'gamma')",
        BENCH_TABLE,
        $id,
    );
}

function updateSql(int $id): string
{
    return sprintf("UPDATE %s SET StringA = 'updated', IntA = IntA + 1 WHERE Id = %d", BENCH_TABLE, $id);
}

function deleteSql(int $id): string
{
    return sprintf('DELETE FROM %s WHERE Id = %d', BENCH_TABLE, $id);
}

function randomOperationId(): int
{
    return random_int(1_000_000, 2_000_000_000);
}
