<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Unary call with per-rev client/channel construction. This is the comparison
 * line for PHP-FPM style request boundaries where php-grpc-lite cannot carry
 * a Channel object to the next PHP request.
 */
final class ColdUnaryBench
{
    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchConstructClientAndSayHello(): void
    {
        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('bench');

        [$response, $status] = $client->SayHello($request)->wait();
        if ($status->code !== \Grpc\STATUS_OK || $response === null) {
            throw new \RuntimeException("cold unary failed: {$status->details}");
        }
        unset($client);
        gc_collect_cycles();
    }
}
