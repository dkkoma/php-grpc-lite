<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Server-streaming throughput against the local Go test-server, using
 * BenchServerStream so we can dial in message count, per-message payload
 * size, and per-message server delay independently.
 *
 * Three sliced views:
 *   - benchByMessageCount: scales count, fixes payload + zero delay
 *     (per-frame overhead)
 *   - benchByPayloadBytes: scales per-message payload, fixes count + zero
 *     delay (per-byte decode cost)
 *   - benchPacedDelivery: scales per-message server delay, fixes count +
 *     payload (server-paced delivery, simulates real server cadence)
 */
#[Bench\BeforeMethods('setUp')]
final class ServerStreamingBench
{
    private GreeterClient $client;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    /** @return iterable<string, array{count: int}> */
    public function provideMessageCounts(): iterable
    {
        yield 'count_10'   => ['count' => 10];
        yield 'count_100'  => ['count' => 100];
        yield 'count_1000' => ['count' => 1000];
    }

    #[Bench\ParamProviders('provideMessageCounts')]
    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchByMessageCount(array $params): void
    {
        $req = new BenchRequest();
        $req->setMessageCount($params['count']);
        $req->setPayloadBytes(100); // fixed small payload
        $this->drain($req);
    }

    /** @return iterable<string, array{bytes: int}> */
    public function providePayloadBytes(): iterable
    {
        yield 'bytes_0'    => ['bytes' => 0];
        yield 'bytes_100'  => ['bytes' => 100];
        yield 'bytes_1k'   => ['bytes' => 1024];
        yield 'bytes_10k'  => ['bytes' => 10 * 1024];
    }

    #[Bench\ParamProviders('providePayloadBytes')]
    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchByPayloadBytes(array $params): void
    {
        $req = new BenchRequest();
        $req->setMessageCount(100); // fixed count
        $req->setPayloadBytes($params['bytes']);
        $this->drain($req);
    }

    /** @return iterable<string, array{delay_ms: int}> */
    public function providePerMessageDelays(): iterable
    {
        yield 'delay_0ms'   => ['delay_ms' => 0];
        yield 'delay_1ms'   => ['delay_ms' => 1];
        yield 'delay_10ms'  => ['delay_ms' => 10];
    }

    #[Bench\ParamProviders('providePerMessageDelays')]
    #[Bench\Revs(5), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchPacedDelivery(array $params): void
    {
        $req = new BenchRequest();
        $req->setMessageCount(10);
        $req->setPayloadBytes(100);
        $req->setServerDelayMs($params['delay_ms']);
        $this->drain($req);
    }

    private function drain(BenchRequest $req): void
    {
        $count = 0;
        foreach ($this->client->BenchServerStream($req)->responses() as $_reply) {
            $count++;
        }
        if ($count !== $req->getMessageCount()) {
            throw new \RuntimeException(
                "expected {$req->getMessageCount()} messages, got $count"
            );
        }
    }
}
