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
 * Step 4: unary ExecuteSql against the data plane. Uses a single-use
 * read-only transaction (omitted, the default) for SELECT 1.
 */
#[Group('integration')]
#[Group('spanner')]
final class ExecuteSqlTest extends TestCase
{
    private const INSTANCE_ID = 'phpgrpclite-step4';
    private const DATABASE_ID = 'step4db';

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

    public function testCreateSessionThenSelectLiteral(): void
    {
        // CreateSession
        $sreq = new CreateSessionRequest();
        $sreq->setDatabase($this->databaseName);
        [$session, $sstatus] = $this->spanner->CreateSession($sreq)->wait();
        self::assertSame(\Grpc\STATUS_OK, $sstatus->code, 'CreateSession failed: ' . $sstatus->details);
        self::assertNotNull($session);
        $this->sessionName = $session->getName();
        self::assertStringStartsWith($this->databaseName . '/sessions/', $this->sessionName);

        // ExecuteSql
        $req = new ExecuteSqlRequest();
        $req->setSession($this->sessionName);
        $req->setSql('SELECT 1 AS n');
        [$result, $status] = $this->spanner->ExecuteSql($req)->wait();
        self::assertSame(\Grpc\STATUS_OK, $status->code, 'ExecuteSql failed: ' . $status->details);
        self::assertNotNull($result);

        $rows = iterator_to_array($result->getRows());
        self::assertCount(1, $rows);
        $values = iterator_to_array($rows[0]->getValues());
        self::assertCount(1, $values);
        // Spanner returns numbers as strings; "1" expected
        self::assertSame('1', $values[0]->getStringValue());
    }
}
