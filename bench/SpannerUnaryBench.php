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
 * Realistic unary latency: ExecuteSql against the Spanner emulator.
 * Talks to a real C++ server and decodes a real ResultSet — closer to
 * what production traffic looks like than the helloworld floor.
 */
#[Bench\BeforeMethods('setUp')]
#[Bench\AfterMethods('tearDown')]
final class SpannerUnaryBench
{
    private const INSTANCE_ID = 'phpgrpclite-bench';
    private const DATABASE_ID = 'benchdb';

    private SpannerGrpcClient $spanner;
    private InstanceAdminGrpcClient $instanceAdmin;
    private DatabaseAdminGrpcClient $databaseAdmin;
    private string $sessionName = '';
    private ExecuteSqlRequest $request;

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

        $this->request = new ExecuteSqlRequest();
        $this->request->setSession($this->sessionName);
        $this->request->setSql('SELECT 1 AS n');
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

    #[Bench\Revs(50), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchExecuteSql(): void
    {
        $this->spanner->ExecuteSql($this->request)->wait();
    }
}
