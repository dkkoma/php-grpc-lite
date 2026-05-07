<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class HttpValidationTest extends TestCase
{
    private GreeterClient $client;

    protected function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testUnaryRejectsNonGrpcContentType(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNKNOWN, $status->code);
        self::assertSame('invalid gRPC content-type: text/plain', $status->details);
    }

    public function testUnaryRejectsGrpcContentTypePrefixLookalike(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, [
            'x-bench-content-type' => ['application/grpcfoo'],
            'x-bench-grpc-status' => ['0'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNKNOWN, $status->code);
        self::assertSame('invalid gRPC content-type: application/grpcfoo', $status->details);
    }

    public function testUnaryRejectsGrpcStatusWithLeadingZero(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, [
            'x-bench-grpc-status' => ['01'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNKNOWN, $status->code);
        self::assertSame('invalid grpc-status trailer', $status->details);
    }

    public function testServerStreamingRejectsNonGrpcContentType(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(10);

        $call = $this->client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_UNKNOWN, $call->getStatus()->code);
        self::assertSame('invalid gRPC content-type: text/plain', $call->getStatus()->details);
    }
}
