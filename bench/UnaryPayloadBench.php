<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Unary call cost as a function of response payload size. Server returns a
 * BenchReply with `payload_bytes` zero bytes — measures protobuf decode +
 * gRPC framing per-byte cost on top of the unary baseline.
 */
#[Bench\BeforeMethods('setUp')]
final class UnaryPayloadBench
{
    private GreeterClient $client;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    /** @return iterable<string, array{bytes: int}> */
    public function providePayloadSizes(): iterable
    {
        yield 'bytes_0'      => ['bytes' => 0];
        yield 'bytes_100'    => ['bytes' => 100];
        yield 'bytes_1k'     => ['bytes' => 1024];
        yield 'bytes_10k'    => ['bytes' => 10 * 1024];
        yield 'bytes_100k'   => ['bytes' => 100 * 1024];
    }

    #[Bench\ParamProviders('providePayloadSizes')]
    #[Bench\Revs(50), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchBenchUnary(array $params): void
    {
        $req = new BenchRequest();
        $req->setPayloadBytes($params['bytes']);
        $this->client->BenchUnary($req)->wait();
    }
}
