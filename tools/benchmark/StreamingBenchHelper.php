<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Benchmark;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

final class StreamingBenchHelper
{
    /** @param array<string, mixed> $options */
    public static function client(string $target, array $options = []): GreeterClient
    {
        return new GreeterClient($target, $options + [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public static function request(int $messageCount, int $payloadBytes = 100, int $serverDelayMs = 0): BenchRequest
    {
        $request = new BenchRequest();
        $request->setMessageCount($messageCount);
        $request->setPayloadBytes($payloadBytes);
        $request->setServerDelayMs($serverDelayMs);

        return $request;
    }

    public static function drain(GreeterClient $client, BenchRequest $request, int $sleepUs = 0): int
    {
        $count = 0;
        foreach ($client->BenchServerStream($request)->responses() as $_reply) {
            $count++;
            if ($sleepUs > 0) {
                usleep($sleepUs);
            }
        }

        if ($count !== $request->getMessageCount()) {
            throw new \RuntimeException("expected {$request->getMessageCount()} messages, got $count");
        }

        return $count;
    }

}
