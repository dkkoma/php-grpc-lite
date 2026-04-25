<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class ServerStreamingTest extends TestCase
{
    private const TARGET = 'test-server:50051';

    public function testSayManyHellosYieldsAllReplies(): void
    {
        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('Stream');

        $call = $client->SayManyHellos($request);

        $messages = [];
        foreach ($call->responses() as $reply) {
            $messages[] = $reply->getMessage();
        }

        self::assertSame(
            ['Hello #1, Stream', 'Hello #2, Stream', 'Hello #3, Stream', 'Hello #4, Stream', 'Hello #5, Stream'],
            $messages,
        );

        $status = $call->getStatus();
        self::assertSame(\Grpc\STATUS_OK, $status->code);
    }

    public function testYieldsAreIncrementalNotBatched(): void
    {
        // The server sleeps 100ms between sends. If our Generator truly
        // yields per-frame, we should see at least ~400ms total elapsed
        // across the 5 yields and the gap between consecutive yields
        // should approximate the server's 100ms cadence.
        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('Pace');

        $call = $client->SayManyHellos($request);

        $start = hrtime(true);
        $stamps = [];
        foreach ($call->responses() as $reply) {
            $stamps[] = (hrtime(true) - $start) / 1e6;
        }

        self::assertCount(5, $stamps);
        // First yield is fast (server sends immediately).
        self::assertLessThan(50.0, $stamps[0], "first yield should arrive quickly");
        // Subsequent yields should be paced ~100ms apart.
        for ($i = 1; $i < 5; $i++) {
            $gap = $stamps[$i] - $stamps[$i - 1];
            self::assertGreaterThan(70.0, $gap, "yield gap at frame $i too small (batched?)");
            self::assertLessThan(150.0, $gap, "yield gap at frame $i too large");
        }
    }
}
