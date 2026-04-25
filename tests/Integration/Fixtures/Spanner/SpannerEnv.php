<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures\Spanner;

use Google\Cloud\Spanner\Admin\Database\V1\CreateDatabaseRequest;
use Google\Cloud\Spanner\Admin\Database\V1\DropDatabaseRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\CreateInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\DeleteInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\Instance;
use Grpc\ChannelCredentials;

/**
 * Test-only helpers for bringing a Spanner emulator instance + database
 * up and down. Each test class that needs a database calls
 * `createInstance` / `createDatabase` in setUp and `deleteInstance` in
 * tearDown, using a unique instanceId per class to keep parallel test
 * runs isolated.
 *
 * The emulator completes all admin LROs synchronously, so we don't bother
 * polling the operation.
 */
final class SpannerEnv
{
    public const TARGET = 'spanner-emulator:9010';
    public const PROJECT = 'projects/test-project';
    public const INSTANCE_CONFIG = 'projects/test-project/instanceConfigs/emulator-config';

    public static function options(): array
    {
        return ['credentials' => ChannelCredentials::createInsecure()];
    }

    public static function createInstance(InstanceAdminGrpcClient $admin, string $instanceId): string
    {
        self::deleteInstance($admin, $instanceId);

        $instance = new Instance();
        $instance->setConfig(self::INSTANCE_CONFIG);
        $instance->setDisplayName($instanceId);
        $instance->setNodeCount(1);

        $req = new CreateInstanceRequest();
        $req->setParent(self::PROJECT);
        $req->setInstanceId($instanceId);
        $req->setInstance($instance);

        [, $status] = $admin->CreateInstance($req)->wait();
        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("CreateInstance failed: $status->details");
        }
        return self::PROJECT . '/instances/' . $instanceId;
    }

    public static function deleteInstance(InstanceAdminGrpcClient $admin, string $instanceId): void
    {
        $req = new DeleteInstanceRequest();
        $req->setName(self::PROJECT . '/instances/' . $instanceId);
        $admin->DeleteInstance($req)->wait();  // ignore NOT_FOUND
    }

    public static function createDatabase(
        DatabaseAdminGrpcClient $admin,
        string $instanceName,
        string $databaseId,
        array $extraStatements = [],
    ): string {
        self::dropDatabase($admin, $instanceName, $databaseId);

        $req = new CreateDatabaseRequest();
        $req->setParent($instanceName);
        $req->setCreateStatement('CREATE DATABASE `' . $databaseId . '`');
        if ($extraStatements !== []) {
            $req->setExtraStatements($extraStatements);
        }

        [, $status] = $admin->CreateDatabase($req)->wait();
        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("CreateDatabase failed: $status->details");
        }
        return $instanceName . '/databases/' . $databaseId;
    }

    public static function dropDatabase(
        DatabaseAdminGrpcClient $admin,
        string $instanceName,
        string $databaseId,
    ): void {
        $req = new DropDatabaseRequest();
        $req->setDatabase($instanceName . '/databases/' . $databaseId);
        $admin->DropDatabase($req)->wait();  // ignore NOT_FOUND
    }
}
