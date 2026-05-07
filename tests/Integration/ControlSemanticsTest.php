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

    public function testDroppedServerStreamDoesNotPoisonConnection(): void
    {
        $client = $this->client();
        $streamRequest = new BenchRequest();
        $streamRequest->setMessageCount(3);
        $streamRequest->setPayloadBytes(10);

        $streamCall = $client->BenchServerStream($streamRequest);
        $responses = $streamCall->responses();
        foreach ($responses as $_reply) {
            break;
        }
        unset($responses, $streamCall);
        gc_collect_cycles();

        [$response, $status] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNotNull($response);
        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
    }

    public function testChannelCloseWaitsForLiveClosedServerStreamResource(): void
    {
        $client = $this->client();
        $streamRequest = new BenchRequest();
        $streamRequest->setMessageCount(1);
        $streamRequest->setPayloadBytes(10);

        $streamCall = $client->BenchServerStream($streamRequest);
        $count = 0;
        foreach ($streamCall->responses() as $_reply) {
            $count++;
        }

        $client->close();
        $status = $streamCall->getStatus();

        self::assertSame(1, $count);
        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
    }

    private function client(): GreeterClient
    {
        return new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }
}
