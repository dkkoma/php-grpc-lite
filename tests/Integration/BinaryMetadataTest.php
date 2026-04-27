<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class BinaryMetadataTest extends TestCase
{
    public function testBinaryMetadataRoundTripUsesRawPhpValues(): void
    {
        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $values = ["\x00\x01\xff,a"];

        $call = $client->BenchUnary(new BenchRequest(), [
            'x-bench-echo-bin' => $values,
        ]);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-bin'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-bin'] ?? null);
    }
}
