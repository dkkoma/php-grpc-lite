<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Regression smoke for the high-message-count server streaming path.
 *
 * This keeps CI/local baseline checks focused on the per-message path without
 * running every ServerStreamingBench parameter combination.
 */
#[Bench\BeforeMethods('setUp')]
final class ServerStreamingCount1000Bench
{
    private GreeterClient $client;
    private BenchRequest $request;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $this->request = new BenchRequest();
        $this->request->setMessageCount(1000);
        $this->request->setPayloadBytes(100);
    }

    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchServerStreamingCount1000(): void
    {
        $count = 0;
        foreach ($this->client->BenchServerStream($this->request)->responses() as $_reply) {
            $count++;
        }
        if ($count !== 1000) {
            throw new \RuntimeException("expected 1000 messages, got $count");
        }
    }
}
