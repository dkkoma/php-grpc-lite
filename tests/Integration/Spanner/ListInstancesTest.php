<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Spanner;

use Google\Cloud\Spanner\Admin\Instance\V1\ListInstancesRequest;
use Grpc\ChannelCredentials;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * Step 1 of the staged Spanner emulator verification plan: prove that our
 * BaseStub talks correctly to the Spanner emulator using the real
 * google-cloud-spanner generated request/response message types.
 *
 * No bootstrap is required — a fresh emulator returns an empty instances
 * list for any project.
 */
#[Group('integration')]
#[Group('spanner')]
final class ListInstancesTest extends TestCase
{
    private const TARGET = 'spanner-emulator:9010';

    public function testListInstancesOnFreshEmulatorReturnsEmptyList(): void
    {
        $client = new InstanceAdminGrpcClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new ListInstancesRequest();
        $request->setParent('projects/test-project');

        [$response, $status] = $client->ListInstances($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, 'gRPC call failed: ' . $status->details);
        self::assertNotNull($response);
        self::assertCount(0, iterator_to_array($response->getInstances()));
    }
}
