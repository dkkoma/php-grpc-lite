<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Unary calls where the server simulates real per-request processing time
 * (HelloRequest.delay_ms). The point is *not* to measure per-call client
 * overhead — that's UnaryLatencyBench's job. Instead this measures whether
 * the threading / event-loop model affects total wall-clock when server
 * time dominates, which is what a large real application typically faces.
 *
 * Expectation: when server takes ~10ms, ext-grpc's worker-thread + event
 * engine model and our libcurl-based blocking model should both end up
 * very close to (server_time × N + tiny constant).
 */
#[Bench\BeforeMethods('setUp')]
final class UnaryDelayBench
{
    private GreeterClient $client;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    /** Single call where the server sleeps 10ms before responding. */
    #[Bench\Revs(50), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchSingleCallWithServerDelay10ms(): void
    {
        $req = new HelloRequest();
        $req->setName('bench');
        $req->setDelayMs(10);
        $this->client->SayHello($req)->wait();
    }

    /** 10 sequential calls, each with 10ms server-side processing. */
    #[Bench\Revs(5), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchSequentialTenCallsServerDelay10ms(): void
    {
        for ($i = 0; $i < 10; $i++) {
            $req = new HelloRequest();
            $req->setName('bench');
            $req->setDelayMs(10);
            $this->client->SayHello($req)->wait();
        }
    }
}
