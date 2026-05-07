<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class ControlSemanticsTest extends TestCase
{
    public function testUnaryCancelBeforeWaitReturnsCancelledStatus(): void
    {
        $client = $this->client();
        $call = $client->BenchUnary(new BenchRequest());
        $call->cancel();

        [$response, $status] = $call->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_CANCELLED, $status->code);
    }

    public function testServerStreamingCancelBeforeIterationReturnsCancelledStatus(): void
    {
        $client = $this->client();
        $call = $client->BenchServerStream(new BenchRequest());
        $call->cancel();

        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_CANCELLED, $call->getStatus()->code);
    }

    public function testUnaryMalformedResponseFrameReturnsFailure(): void
    {
        $client = $this->client('test-server:50057');

        [, $status] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNotSame(\Grpc\STATUS_OK, $status->code);
    }

    public function testUnaryServerSentRstStreamIsStreamLocal(): void
    {
        $client = $this->client('test-server:50058');

        [$firstResponse, $firstStatus] = $client->BenchUnary(new BenchRequest())->wait();
        [$secondResponse, $secondStatus] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNull($firstResponse);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $firstStatus->code);
        self::assertNotNull($secondResponse);
        self::assertSame(\Grpc\STATUS_OK, $secondStatus->code, $secondStatus->details);
    }

    public function testServerStreamingServerSentRstStreamIsStreamLocal(): void
    {
        $client = $this->client('test-server:50059');

        $firstCall = $client->BenchServerStream(new BenchRequest());
        $firstCount = 0;
        foreach ($firstCall->responses() as $_reply) {
            $firstCount++;
        }

        $secondCall = $client->BenchServerStream(new BenchRequest());
        $secondCount = 0;
        foreach ($secondCall->responses() as $_reply) {
            $secondCount++;
        }

        self::assertSame(0, $firstCount);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $firstCall->getStatus()->code);
        self::assertSame(1, $secondCount);
        self::assertSame(\Grpc\STATUS_OK, $secondCall->getStatus()->code, $secondCall->getStatus()->details);
    }

    public function testUnaryGoAwayRefusedStreamReturnsUnavailable(): void
    {
        $client = $this->client('test-server:50060');

        [$response, $status] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code, $status->details);
    }

    private function client(string $target = 'test-server:50051'): GreeterClient
    {
        return new GreeterClient($target, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }
}
