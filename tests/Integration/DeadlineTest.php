<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class DeadlineTest extends TestCase
{
    private const TARGET = 'test-server:50051';

    private GreeterClient $client;

    protected function setUp(): void
    {
        $this->client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testUnaryDeadlineExceededIsEnforcedClientSide(): void
    {
        $request = new BenchRequest();
        $request->setServerDelayMs(100);

        $start = hrtime(true);
        [$response, $status] = $this->client->BenchUnary($request, [], [
            'timeout' => 10_000,
        ])->wait();
        $elapsedMs = (hrtime(true) - $start) / 1_000_000;

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, $status->details);
        self::assertLessThan(80.0, $elapsedMs);
    }

    public function testServerStreamingDeadlineExceededIsEnforcedClientSide(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(10);
        $request->setPayloadBytes(100);
        $request->setServerDelayMs(50);

        $call = $this->client->BenchServerStream($request, [], [
            'timeout' => 20_000,
        ]);

        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertGreaterThanOrEqual(1, $count);
        self::assertLessThan(10, $count);
        self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $call->getStatus()->code);
    }

    public function testServerStreamingImmediateDeadlineReturnsStatus(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(1);

        $call = $this->client->BenchServerStream($request, [], [
            'timeout' => 1,
        ]);

        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $call->getStatus()->code);
    }
}
