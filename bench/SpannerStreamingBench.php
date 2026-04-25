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
 * Server-streaming throughput: ExecuteStreamingSql with synthetic UNNEST
 * queries of varying row counts. Exercises the curl_multi Generator and
 * the per-frame deserialize hot path.
 */
#[Bench\BeforeMethods('setUp')]
#[Bench\AfterMethods('tearDown')]
final class SpannerStreamingBench
{
    private const INSTANCE_ID = 'phpgrpclite-bench-stream';
    private const DATABASE_ID = 'benchstreamdb';

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
        yield 'rows_10'   => ['rows' => 10];
        yield 'rows_100'  => ['rows' => 100];
        yield 'rows_1000' => ['rows' => 1000];
    }

    #[Bench\ParamProviders('provideRowCounts')]
    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchExecuteStreamingSql(array $params): void
    {
        $req = new ExecuteSqlRequest();
        $req->setSession($this->sessionName);
        $req->setSql(sprintf(
            'SELECT n FROM UNNEST(GENERATE_ARRAY(1, %d)) AS n',
            $params['rows'],
        ));

        $count = 0;
        foreach ($this->spanner->ExecuteStreamingSql($req)->responses() as $partial) {
            foreach ($partial->getValues() as $_v) {
                $count++;
            }
        }
        if ($count !== $params['rows']) {
            throw new \RuntimeException("expected {$params['rows']} values, got $count");
        }
    }
}
