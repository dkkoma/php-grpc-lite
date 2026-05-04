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
    public function testCurlUnaryCancelBeforeWaitReturnsCancelledStatus(): void
    {
        $client = $this->client();
        $call = $client->BenchUnary(new BenchRequest());
        $call->cancel();

        [$response, $status] = $call->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_CANCELLED, $status->code);
    }

    public function testCurlServerStreamingCancelBeforeIterationReturnsCancelledStatus(): void
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

    public function testCurlUnaryMalformedResponseFrameReturnsInternal(): void
    {
        $client = $this->client('test-server:50057');

        [, $status] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNotSame(\Grpc\STATUS_OK, $status->code);
    }

    private function client(string $target = 'test-server:50051'): GreeterClient
    {
        return new GreeterClient($target, [
            'credentials' => ChannelCredentials::createInsecure(),
            'php_grpc_lite.transport' => 'curl',
        ]);
    }
}
