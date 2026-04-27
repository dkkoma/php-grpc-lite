<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Server-streaming under a deliberately slow consumer.
 *
 * This is not a raw throughput benchmark. The point is to observe how much
 * memory and elapsed time increase when the application drains responses more
 * slowly than the server can emit them.
 */
#[Bench\BeforeMethods('setUp')]
final class ServerStreamingSlowConsumerBench
{
    private GreeterClient $client;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    /** @return iterable<string, array{count: int, payload_bytes: int, sleep_us: int}> */
    public function provideSlowConsumerCases(): iterable
    {
        yield 'sleep_100us_payload_100b' => [
            'count' => 1000,
            'payload_bytes' => 100,
            'sleep_us' => 100,
        ];
        yield 'sleep_1ms_payload_100b' => [
            'count' => 100,
            'payload_bytes' => 100,
            'sleep_us' => 1000,
        ];
        yield 'sleep_1ms_payload_10k' => [
            'count' => 100,
            'payload_bytes' => 10 * 1024,
            'sleep_us' => 1000,
        ];
    }

    #[Bench\ParamProviders('provideSlowConsumerCases')]
    #[Bench\Revs(5), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchSlowConsumer(array $params): void
    {
        $req = new BenchRequest();
        $req->setMessageCount($params['count']);
        $req->setPayloadBytes($params['payload_bytes']);

        $count = 0;
        foreach ($this->client->BenchServerStream($req)->responses() as $_reply) {
            $count++;
            usleep($params['sleep_us']);
        }

        if ($count !== $req->getMessageCount()) {
            throw new \RuntimeException(
                "expected {$req->getMessageCount()} messages, got $count"
            );
        }
    }
}
