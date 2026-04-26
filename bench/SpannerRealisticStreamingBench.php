<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Google\Cloud\Spanner\V1\CreateSessionRequest;
use Google\Cloud\Spanner\V1\DeleteSessionRequest;
use Google\Cloud\Spanner\V1\ExecuteSqlRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\DatabaseAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerEnv;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerGrpcClient;

/**
 * Server-streaming throughput with a realistic, non-empty payload:
 * 2 STRING + 2 TIMESTAMP columns per row, ~140 bytes serialized per row.
 * Rows are synthesized via UNNEST + Spanner functions — wire-format
 * identical to a SELECT against a real table.
 *
 * Complements SpannerStreamingBench (bare INT64) so per-row marginal
 * cost can be split into "framing/decode constant" + "per-byte cost".
 */
#[Bench\BeforeMethods('setUp')]
#[Bench\AfterMethods('tearDown')]
final class SpannerRealisticStreamingBench
{
    private const INSTANCE_ID = 'phpgrpclite-bench-realistic';
    private const DATABASE_ID = 'benchrealdb';

    private SpannerGrpcClient $spanner;
    private InstanceAdminGrpcClient $instanceAdmin;
    private DatabaseAdminGrpcClient $databaseAdmin;
    private string $sessionName = '';

    public function setUp(): void
    {
        $this->instanceAdmin = new InstanceAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->databaseAdmin = new DatabaseAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->spanner = new SpannerGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());

        $instance = SpannerEnv::createInstance($this->instanceAdmin, self::INSTANCE_ID);
        $database = SpannerEnv::createDatabase($this->databaseAdmin, $instance, self::DATABASE_ID);

        $sreq = new CreateSessionRequest();
        $sreq->setDatabase($database);
        [$session] = $this->spanner->CreateSession($sreq)->wait();
        $this->sessionName = $session->getName();
    }

    public function tearDown(): void
    {
        if ($this->sessionName !== '') {
            $req = new DeleteSessionRequest();
            $req->setName($this->sessionName);
            $this->spanner->DeleteSession($req)->wait();
        }
        SpannerEnv::deleteInstance($this->instanceAdmin, self::INSTANCE_ID);
    }

    /** @return iterable<string, array{rows: int}> */
    public function provideRowCounts(): iterable
    {
        yield 'rows_100'  => ['rows' => 100];
        yield 'rows_1000' => ['rows' => 1000];
    }

    #[Bench\ParamProviders('provideRowCounts')]
    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchExecuteStreamingSqlRealistic(array $params): void
    {
        $req = new ExecuteSqlRequest();
        $req->setSession($this->sessionName);
        $req->setSql(sprintf(
            "SELECT
                CONCAT('first_name_', CAST(n AS STRING))                           AS first_name,
                CONCAT('last_name_',  CAST(n AS STRING), REPEAT('-x', 30))         AS last_name,
                TIMESTAMP_ADD(TIMESTAMP '2026-01-01 00:00:00', INTERVAL n SECOND)  AS created_at,
                TIMESTAMP_SUB(TIMESTAMP '2026-12-31 23:59:59', INTERVAL n SECOND)  AS updated_at
            FROM UNNEST(GENERATE_ARRAY(1, %d)) AS n",
            $params['rows'],
        ));

        $cells = 0;
        foreach ($this->spanner->ExecuteStreamingSql($req)->responses() as $partial) {
            foreach ($partial->getValues() as $_v) {
                $cells++;
            }
        }
        $expected = $params['rows'] * 4; // 4 columns per row
        if ($cells !== $expected) {
            throw new \RuntimeException("expected {$expected} values, got $cells");
        }
    }
}
