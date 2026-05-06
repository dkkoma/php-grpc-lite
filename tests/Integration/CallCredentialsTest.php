<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class CallCredentialsTest extends TestCase
{
    public function testCreateFromPluginAddsPerCallMetadata(): void
    {
        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
        $callCredentialsCallback = static function (string $serviceUrl, string $methodName): array {
            self::assertStringStartsWith('http://test-server:50051/', $serviceUrl);
            self::assertSame('/helloworld.Greeter/BenchUnary', $methodName);
            return ['x-bench-echo-ascii' => 'from-plugin'];
        };

        $call = $client->BenchUnary(new BenchRequest(), [], [
            'call_credentials_callback' => $callCredentialsCallback,
        ]);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame(['from-plugin'], $call->getMetadata()['x-bench-initial-ascii'] ?? null);
    }
}
