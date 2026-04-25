<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Spanner;

use Google\Cloud\Spanner\V1\CreateSessionRequest;
use Google\Cloud\Spanner\V1\DeleteSessionRequest;
use Google\Cloud\Spanner\V1\ExecuteSqlRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\DatabaseAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerEnv;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerGrpcClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * Step 5 (the highlight): server-streaming ExecuteStreamingSql.
 * Uses an UNNEST(GENERATE_ARRAY(...)) query so no table or seed data
 * is required — the rows are synthesized server-side.
 */
#[Group('integration')]
#[Group('spanner')]
final class ExecuteStreamingSqlTest extends TestCase
{
    private const INSTANCE_ID = 'phpgrpclite-step5';
    private const DATABASE_ID = 'step5db';

    private InstanceAdminGrpcClient $instanceAdmin;
    private DatabaseAdminGrpcClient $databaseAdmin;
    private SpannerGrpcClient $spanner;
    private string $databaseName;
    private string $sessionName = '';

    protected function setUp(): void
    {
        $this->instanceAdmin = new InstanceAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->databaseAdmin = new DatabaseAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->spanner = new SpannerGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());

        $instance = SpannerEnv::createInstance($this->instanceAdmin, self::INSTANCE_ID);
        $this->databaseName = SpannerEnv::createDatabase($this->databaseAdmin, $instance, self::DATABASE_ID);

        $sreq = new CreateSessionRequest();
        $sreq->setDatabase($this->databaseName);
        [$session, $sstatus] = $this->spanner->CreateSession($sreq)->wait();
        if ($sstatus->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException('CreateSession failed: ' . $sstatus->details);
        }
        $this->sessionName = $session->getName();
    }

    protected function tearDown(): void
    {
        if ($this->sessionName !== '') {
            $req = new DeleteSessionRequest();
            $req->setName($this->sessionName);
            $this->spanner->DeleteSession($req)->wait();
        }
        SpannerEnv::deleteInstance($this->instanceAdmin, self::INSTANCE_ID);
    }

    public function testStreamingSelectYieldsAllRows(): void
    {
        $req = new ExecuteSqlRequest();
        $req->setSession($this->sessionName);
        $req->setSql('SELECT n FROM UNNEST(GENERATE_ARRAY(1, 10)) AS n ORDER BY n');

        $call = $this->spanner->ExecuteStreamingSql($req);

        $partialCount = 0;
        $sawMetadata = false;
        $values = [];

        foreach ($call->responses() as $partial) {
            $partialCount++;
            if ($partial->hasMetadata()) {
                $sawMetadata = true;
            }
            foreach ($partial->getValues() as $v) {
                // Spanner returns INT64 values as strings inside Value
                $values[] = $v->getStringValue();
            }
        }

        $status = $call->getStatus();
        self::assertSame(\Grpc\STATUS_OK, $status->code, 'streaming failed: ' . $status->details);

        self::assertGreaterThanOrEqual(1, $partialCount, 'expected at least one PartialResultSet');
        self::assertTrue($sawMetadata, 'first PartialResultSet should carry the metadata');
        self::assertSame(['1', '2', '3', '4', '5', '6', '7', '8', '9', '10'], $values);
    }

    public function testLargeStreamingResultIsAccumulatedCorrectly(): void
    {
        // 1000 rows × 1 column. The emulator may chunk this across
        // multiple PartialResultSets; we verify the total reconstructs.
        $req = new ExecuteSqlRequest();
        $req->setSession($this->sessionName);
        $req->setSql('SELECT n FROM UNNEST(GENERATE_ARRAY(1, 1000)) AS n ORDER BY n');

        $call = $this->spanner->ExecuteStreamingSql($req);

        $partialCount = 0;
        $totalValues = 0;
        foreach ($call->responses() as $partial) {
            $partialCount++;
            $totalValues += count(iterator_to_array($partial->getValues()));
        }

        self::assertSame(\Grpc\STATUS_OK, $call->getStatus()->code);
        self::assertSame(1000, $totalValues);
        // Surface partialCount via a benign assertion so it shows up in test output
        // even when a single PartialResultSet covers the whole result.
        self::assertGreaterThanOrEqual(1, $partialCount);
    }
}
