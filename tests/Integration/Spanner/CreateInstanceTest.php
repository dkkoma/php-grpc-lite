<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Spanner;

use Google\Cloud\Spanner\Admin\Instance\V1\CreateInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\DeleteInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\Instance;
use Google\Cloud\Spanner\Admin\Instance\V1\ListInstancesRequest;
use Grpc\ChannelCredentials;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * Step 2 of the staged Spanner emulator verification plan: a write-side
 * unary RPC. CreateInstance is a long-running operation; on the emulator
 * it completes synchronously (operation.done == true on first response).
 */
#[Group('integration')]
#[Group('spanner')]
final class CreateInstanceTest extends TestCase
{
    private const TARGET = 'spanner-emulator:9010';
    private const PROJECT = 'projects/test-project';
    private const INSTANCE_ID = 'phpgrpclite-step2';
    private const INSTANCE_CONFIG = 'projects/test-project/instanceConfigs/emulator-config';

    private InstanceAdminGrpcClient $client;

    protected function setUp(): void
    {
        $this->client = new InstanceAdminGrpcClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
        $this->deleteInstanceIfExists();
    }

    protected function tearDown(): void
    {
        $this->deleteInstanceIfExists();
    }

    public function testCreateInstanceReturnsCompletedOperation(): void
    {
        $instance = new Instance();
        $instance->setConfig(self::INSTANCE_CONFIG);
        $instance->setDisplayName('Phase 0 step 2');
        $instance->setNodeCount(1);

        $request = new CreateInstanceRequest();
        $request->setParent(self::PROJECT);
        $request->setInstanceId(self::INSTANCE_ID);
        $request->setInstance($instance);

        [$operation, $status] = $this->client->CreateInstance($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, 'CreateInstance failed: ' . $status->details);
        self::assertNotNull($operation);
        self::assertTrue($operation->getDone(), 'emulator should complete LRO synchronously');
        self::assertFalse($operation->hasError(), 'operation must not carry an error');
        self::assertNotEmpty($operation->getName(), 'operation must have a name');

        // Sanity: the new instance now shows up in ListInstances
        $listReq = new ListInstancesRequest();
        $listReq->setParent(self::PROJECT);
        [$list, $listStatus] = $this->client->ListInstances($listReq)->wait();
        self::assertSame(\Grpc\STATUS_OK, $listStatus->code);

        $names = [];
        foreach ($list->getInstances() as $inst) {
            $names[] = $inst->getName();
        }
        self::assertContains(self::PROJECT . '/instances/' . self::INSTANCE_ID, $names);
    }

    private function deleteInstanceIfExists(): void
    {
        $req = new DeleteInstanceRequest();
        $req->setName(self::PROJECT . '/instances/' . self::INSTANCE_ID);
        $this->client->DeleteInstance($req)->wait();  // ignore status (NOT_FOUND on first run is fine)
    }
}
