<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class CompressionTest extends TestCase
{
    private GreeterClient $client;

    protected function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testUnaryCompressedMessageIsExplicitlyUnsupported(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, [
            'x-bench-grpc-response' => ['compressed-flag'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('compressed gRPC messages are not supported', $status->details);
    }

    public function testUnaryGrpcEncodingIsExplicitlyUnsupported(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, [
            'x-bench-grpc-encoding' => ['gzip'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('unsupported grpc-encoding: gzip', $status->details);
    }

    public function testServerStreamingCompressedMessageIsExplicitlyUnsupported(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(10);

        $call = $this->client->BenchServerStream($request, [
            'x-bench-grpc-response' => ['compressed-flag'],
        ]);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $call->getStatus()->code);
        self::assertSame('compressed gRPC messages are not supported', $call->getStatus()->details);
    }

    public function testCompressedMessageIsRejectedEvenWhenGrpcStatusIsOk(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, [
            'x-bench-grpc-response' => ['compressed-flag'],
            'x-bench-grpc-status' => ['0'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('compressed gRPC messages are not supported', $status->details);
    }

    public function testUnaryCallSucceedsAfterStreamLocalCompressionFailure(): void
    {
        $request = new BenchRequest();
        [, $failedStatus] = $this->client->BenchUnary($request, [
            'x-bench-grpc-response' => ['compressed-flag'],
        ])->wait();

        [$response, $status] = $this->client->BenchUnary(new BenchRequest(), [
            'x-bench-grpc-status' => ['0'],
        ])->wait();

        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $failedStatus->code);
        self::assertNotNull($response);
        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
    }

    public function testServerStreamingCompressedMessageIsRejectedEvenWhenGrpcStatusIsOk(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(10);

        $call = $this->client->BenchServerStream($request, [
            'x-bench-grpc-response' => ['compressed-flag'],
            'x-bench-grpc-status' => ['0'],
        ]);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $call->getStatus()->code);
        self::assertSame('compressed gRPC messages are not supported', $call->getStatus()->details);
    }
}
