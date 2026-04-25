<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Spanner;

use Google\Cloud\Spanner\Admin\Database\V1\CreateDatabaseRequest;
use Google\Cloud\Spanner\Admin\Database\V1\ListDatabasesRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\DatabaseAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerEnv;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * Step 3: CreateDatabase (LRO with DDL).
 */
#[Group('integration')]
#[Group('spanner')]
final class CreateDatabaseTest extends TestCase
{
    private const INSTANCE_ID = 'phpgrpclite-step3';
    private const DATABASE_ID = 'step3db';

    private InstanceAdminGrpcClient $instanceAdmin;
    private DatabaseAdminGrpcClient $databaseAdmin;
    private string $instanceName;

    protected function setUp(): void
    {
        $this->instanceAdmin = new InstanceAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->databaseAdmin = new DatabaseAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
        $this->instanceName = SpannerEnv::createInstance($this->instanceAdmin, self::INSTANCE_ID);
    }

    protected function tearDown(): void
    {
        SpannerEnv::deleteInstance($this->instanceAdmin, self::INSTANCE_ID);
    }

    public function testCreateDatabaseWithSchemaThenAppearsInList(): void
    {
        $req = new CreateDatabaseRequest();
        $req->setParent($this->instanceName);
        $req->setCreateStatement('CREATE DATABASE `' . self::DATABASE_ID . '`');
        $req->setExtraStatements([
            'CREATE TABLE Singers (
                SingerId INT64 NOT NULL,
                FirstName STRING(MAX),
                LastName  STRING(MAX),
            ) PRIMARY KEY (SingerId)',
        ]);

        [$operation, $status] = $this->databaseAdmin->CreateDatabase($req)->wait();
        self::assertSame(\Grpc\STATUS_OK, $status->code, 'CreateDatabase failed: ' . $status->details);
        self::assertNotNull($operation);
        self::assertTrue($operation->getDone());
        self::assertFalse($operation->hasError());

        // Verify the database is now listed
        $listReq = new ListDatabasesRequest();
        $listReq->setParent($this->instanceName);
        [$list, $listStatus] = $this->databaseAdmin->ListDatabases($listReq)->wait();
        self::assertSame(\Grpc\STATUS_OK, $listStatus->code);

        $names = [];
        foreach ($list->getDatabases() as $db) {
            $names[] = $db->getName();
        }
        self::assertContains(
            $this->instanceName . '/databases/' . self::DATABASE_ID,
            $names,
        );
    }
}
