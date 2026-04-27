<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class ErrorSemanticsTest extends TestCase
{
    private GreeterClient $client;

    protected function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testUnaryTrailersOnlyErrorReturnsGrpcStatus(): void
    {
        $request = new BenchRequest();
        [$response, $status] = $this->client->BenchUnary($request, $this->errorMetadata())->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_INVALID_ARGUMENT, $status->code);
        self::assertSame('bench error with spaces', $status->details);
        self::assertArrayHasKey('grpc-status', $status->metadata);
    }

    public function testServerStreamingTrailersOnlyErrorReturnsGrpcStatus(): void
    {
        $request = new BenchRequest();
        $request->setMessageCount(10);
        $request->setPayloadBytes(100);

        $call = $this->client->BenchServerStream($request, $this->errorMetadata());
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        $status = $call->getStatus();
        self::assertSame(\Grpc\STATUS_INVALID_ARGUMENT, $status->code);
        self::assertSame('bench error with spaces', $status->details);
        self::assertArrayHasKey('grpc-status', $status->metadata);
    }

    /** @return array<string, list<string>> */
    private function errorMetadata(): array
    {
        return [
            'x-bench-error-code' => [(string) \Grpc\STATUS_INVALID_ARGUMENT],
            'x-bench-error-message' => ['bench error with spaces'],
        ];
    }
}
