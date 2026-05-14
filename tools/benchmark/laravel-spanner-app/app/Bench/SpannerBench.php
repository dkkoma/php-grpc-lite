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
        $this->ensureInstance($connection);

        if (!$connection->databaseExists()) {
            $connection->createDatabase([$this->benchTableDdl()]);
        } else {
            try {
                $connection->runDdlBatch([$this->benchTableDdl()]);
            } catch (Throwable) {
                // Duplicate table DDL is expected after the first setup request.
            }
        }

        $connection->transaction(function (Connection $connection): void {
            $connection->statement('DELETE FROM ' . self::TABLE . ' WHERE Id >= 1');
            $connection->statement($this->insertSql(1));
        });

        return ['setup' => true];
    }

    public function warmupSessionPool(): array
    {
        $connection = $this->connection();
        $connection->clearSessionPool();
        $created = $connection->warmupSessionPool();
        return ['created_sessions' => $created];
    }

    public function selectOneRow(): array
    {
        $rows = $this->connection()->transaction(function (Connection $connection): array {
            return $connection->select('SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM ' . self::TABLE . ' WHERE Id = ?', [1]);
        });
        if (count($rows) !== 1) {
            throw new RuntimeException('expected 1 row, got ' . count($rows));
        }
        return ['rows' => count($rows)];
    }

    public function dmlInsert(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $affected = $this->connection()->transaction(fn(Connection $connection): int => $connection->affectingStatement($this->insertSql($id)));
        $this->assertAffected($affected);
        return ['affected' => $affected];
    }

    public function dmlUpdate(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $connection = $this->connection();
        $connection->transaction(fn(Connection $connection): bool => $connection->statement($this->insertSql($id)));
        $affected = $connection->transaction(fn(Connection $connection): int => $connection->affectingStatement($this->updateSql($id)));
        $this->assertAffected($affected);
        return ['affected' => $affected];
    }

    public function dmlDelete(): array
    {
        $id = random_int(1_000_000, 2_000_000_000);
        $connection = $this->connection();
        $connection->transaction(fn(Connection $connection): bool => $connection->statement($this->insertSql($id)));
        $affected = $connection->transaction(fn(Connection $connection): int => $connection->affectingStatement($this->deleteSql($id)));
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
}
