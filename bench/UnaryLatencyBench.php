<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Bare unary latency floor: helloworld.Greeter/SayHello over h2c against
 * the local Go test-server. Establishes the minimum per-call cost of the
 * pure-PHP gRPC client (libcurl + framing + HEADER/WRITE callbacks).
 */
#[Bench\BeforeMethods('setUp')]
final class UnaryLatencyBench
{
    private GreeterClient $client;
    private HelloRequest $request;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
        $this->request = new HelloRequest();
        $this->request->setName('bench');
    }

    #[Bench\Revs(50), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchSayHello(): void
    {
        $this->client->SayHello($this->request)->wait();
    }
}
