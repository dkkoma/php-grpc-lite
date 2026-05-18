<?php

declare(strict_types=1);

namespace BenchApp\Bench;

use Colopl\Spanner\Connection;
use Google\Cloud\Spanner\SpannerClient;
use Illuminate\Database\DatabaseManager;
use RuntimeException;
use Throwable;

final class SpannerBench
{
    private const TABLE = 'BenchRows';

    public function __construct(private readonly DatabaseManager $databaseManager)
    {
    }

    public function setupDatabase(): array
    {
        $connection = $this->connection();
        SpannerTraceRecorder::measure('setup.ensure_instance', [], fn (): null => $this->ensureInstance($connection));

        if (!$connection->databaseExists()) {
            SpannerTraceRecorder::measure('setup.create_database', [], fn (): mixed => $connection->createDatabase([$this->benchTableDdl()]));
        } else {
            try {
                SpannerTraceRecorder::measure('setup.run_ddl_batch', [], fn (): mixed => $connection->runDdlBatch([$this->benchTableDdl()]));
            } catch (Throwable) {
                // Duplicate table DDL is expected after the first setup request.
            }
        }

        SpannerTraceRecorder::measure('setup.seed_transaction', [], function () use ($connection): void {
            $connection->transaction(function (Connection $connection): void {
                SpannerTraceRecorder::measure('setup.seed_delete', [], fn (): bool => $connection->statement('DELETE FROM ' . self::TABLE . ' WHERE Id >= 1'));
                SpannerTraceRecorder::measure('setup.seed_insert', [], fn (): bool => $connection->statement($this->insertSql(1)));
            });
        });

        return ['setup' => true];
    }

    public function warmupSessionPool(): array
    {
        $connection = $this->connection();
        SpannerTraceRecorder::measure('session_pool.clear', [], fn (): null => $connection->clearSessionPool());
        $created = SpannerTraceRecorder::measure('session_pool.warmup', [], fn (): int => $connection->warmupSessionPool());
        return ['created_sessions' => $created];
    }

    public function selectOneRow(): array
    {
        $rows = SpannerTraceRecorder::measure(
            'select_1row_10col.transaction',
            [],
            fn (): array => $this->connection()->transaction(
                fn(Connection $connection): array => SpannerTraceRecorder::measure(
                    'select_1row_10col.select',
                    ['row_id' => 1],
                    fn (): array => $this->selectRowsById($connection, 1),
                ),
            ),
        );
        $this->assertRowCount($rows, 1);
        return ['rows' => count($rows)];
    }

    public function mixedTransaction(): array
    {
        $selectedId = random_int(1_000_000, 2_000_000_000);
        $insertedId = random_int(2_000_000_001, PHP_INT_MAX);
        $connection = $this->connection();

        SpannerTraceRecorder::record('mixed.ids', [
            'selected_id' => $selectedId,
            'inserted_id' => $insertedId,
        ]);

        $preInserted = SpannerTraceRecorder::measure(
            'mixed.pre_insert.transaction',
            ['row_id' => $selectedId],
            fn (): int => $connection->transaction(
                fn(Connection $connection): int => SpannerTraceRecorder::measure(
                    'mixed.pre_insert.dml',
                    ['row_id' => $selectedId],
                    fn (): int => $connection->affectingStatement($this->insertSql($selectedId)),
                ),
            ),
        );
        $this->assertAffected($preInserted);

        $result = SpannerTraceRecorder::measure(
            'mixed.main_transaction',
            [
                'selected_id' => $selectedId,
                'inserted_id' => $insertedId,
            ],
            function () use ($connection, $selectedId, $insertedId): array {
                return $connection->transaction(function (Connection $connection) use ($selectedId, $insertedId): array {
                    $firstRows = SpannerTraceRecorder::measure(
                        'mixed.select_1',
                        ['row_id' => $selectedId],
                        fn (): array => $this->selectRowsById($connection, $selectedId),
                    );
                    $this->assertRowCount($firstRows, 1);

                    $secondRows = SpannerTraceRecorder::measure(
                        'mixed.select_2',
                        ['row_id' => $selectedId],
                        fn (): array => $this->selectRowsById($connection, $selectedId),
                    );
                    $this->assertRowCount($secondRows, 1);

                    $updated = SpannerTraceRecorder::measure(
                        'mixed.update',
                        ['row_id' => $selectedId],
                        fn (): int => $connection->affectingStatement($this->updateSql($selectedId)),
                    );
                    $this->assertAffected($updated);

                    $inserted = SpannerTraceRecorder::measure(
                        'mixed.insert',
                        ['row_id' => $insertedId],
                        fn (): int => $connection->affectingStatement($this->insertSql($insertedId)),
                    );
                    $this->assertAffected($inserted);

                    return [
                        'selects' => 2,
                        'selected_rows' => count($firstRows) + count($secondRows),
                        'updated' => $updated,
                        'inserted' => $inserted,
                    ];
                });
            },
        );

        return ['pre_inserted' => $preInserted] + $result;
    }

    public function dmlInsert(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $affected = SpannerTraceRecorder::measure(
            'dml_insert_10col.transaction',
            ['row_id' => $id],
            fn (): int => $this->connection()->transaction(
                fn(Connection $connection): int => SpannerTraceRecorder::measure(
                    'dml_insert_10col.dml',
                    ['row_id' => $id],
                    fn (): int => $connection->affectingStatement($this->insertSql($id)),
                ),
            ),
        );
        $this->assertAffected($affected);
        return ['affected' => $affected];
    }

    public function dmlUpdate(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $connection = $this->connection();
        SpannerTraceRecorder::measure(
            'dml_update_10col.seed_transaction',
            ['row_id' => $id],
            fn (): bool => $connection->transaction(
                fn(Connection $connection): bool => SpannerTraceRecorder::measure(
                    'dml_update_10col.seed_insert',
                    ['row_id' => $id],
                    fn (): bool => $connection->statement($this->insertSql($id)),
                ),
            ),
        );
        $affected = SpannerTraceRecorder::measure(
            'dml_update_10col.transaction',
            ['row_id' => $id],
            fn (): int => $connection->transaction(
                fn(Connection $connection): int => SpannerTraceRecorder::measure(
                    'dml_update_10col.dml',
                    ['row_id' => $id],
                    fn (): int => $connection->affectingStatement($this->updateSql($id)),
                ),
            ),
        );
        $this->assertAffected($affected);
        return ['affected' => $affected];
    }

    public function dmlDelete(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $connection = $this->connection();
        SpannerTraceRecorder::measure(
            'dml_delete_10col.seed_transaction',
            ['row_id' => $id],
            fn (): bool => $connection->transaction(
                fn(Connection $connection): bool => SpannerTraceRecorder::measure(
                    'dml_delete_10col.seed_insert',
                    ['row_id' => $id],
                    fn (): bool => $connection->statement($this->insertSql($id)),
                ),
            ),
        );
        $affected = SpannerTraceRecorder::measure(
            'dml_delete_10col.transaction',
            ['row_id' => $id],
            fn (): int => $connection->transaction(
                fn(Connection $connection): int => SpannerTraceRecorder::measure(
                    'dml_delete_10col.dml',
                    ['row_id' => $id],
                    fn (): int => $connection->affectingStatement($this->deleteSql($id)),
                ),
            ),
        );
        $this->assertAffected($affected);
        return ['affected' => $affected];
    }

    private function connection(): Connection
    {
        $connection = $this->databaseManager->connection('spanner');
        if (!$connection instanceof Connection) {
            throw new RuntimeException('spanner connection was not created');
        }
        return $connection;
    }

    private function ensureInstance(Connection $connection): void
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

    private function benchTableDdl(): string
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

    private function insertSql(int $id): string
    {
        return sprintf(
            "INSERT INTO %s (Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC) VALUES (%d, DATE '2026-05-14', DATE '2026-05-15', 'alpha', 'beta', 123, 456, TRUE, 1.25, 'gamma')",
            self::TABLE,
            $id,
        );
    }

    private function updateSql(int $id): string
    {
        return sprintf("UPDATE %s SET StringA = 'updated', IntA = IntA + 1 WHERE Id = %d", self::TABLE, $id);
    }

    private function deleteSql(int $id): string
    {
        return sprintf('DELETE FROM %s WHERE Id = %d', self::TABLE, $id);
    }

    private function assertAffected(int $affected): void
    {
        if ($affected !== 1) {
            throw new RuntimeException("expected 1 affected row, got $affected");
        }
    }

    private function selectRowsById(Connection $connection, int $id): array
    {
        return $connection->select('SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM ' . self::TABLE . ' WHERE Id = ?', [$id]);
    }

    private function assertRowCount(array $rows, int $expected): void
    {
        if (count($rows) !== $expected) {
            throw new RuntimeException('expected ' . $expected . ' row(s), got ' . count($rows));
        }
    }
}
