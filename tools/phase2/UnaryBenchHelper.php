<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

final class UnaryBenchHelper
{
    public static function client(string $target): GreeterClient
    {
        return new GreeterClient($target, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public static function request(int $payloadBytes = 100, int $serverDelayMs = 0): BenchRequest
    {
        $request = new BenchRequest();
        $request->setPayloadBytes($payloadBytes);
        $request->setServerDelayMs($serverDelayMs);

        return $request;
    }

    public static function call(GreeterClient $client, BenchRequest $request): void
    {
        [$response, $status] = $client->BenchUnary($request)->wait();
        if ($status->code !== \Grpc\STATUS_OK || $response === null) {
            throw new \RuntimeException("BenchUnary failed: {$status->details}");
        }
    }

    /**
     * @param list<int|float> $values
     * @return array{min: int|float, p50: int|float, p95: int|float, p99: int|float, max: int|float}
     */
    public static function percentiles(array $values): array
    {
        if ($values === []) {
            throw new \InvalidArgumentException('percentiles require at least one value');
        }

        sort($values, SORT_NUMERIC);

        return [
            'min' => $values[0],
            'p50' => self::percentile($values, 50.0),
            'p95' => self::percentile($values, 95.0),
            'p99' => self::percentile($values, 99.0),
            'max' => $values[count($values) - 1],
        ];
    }

    /**
     * @param list<int|float> $sortedValues
     */
    private static function percentile(array $sortedValues, float $percentile): int|float
    {
        $index = (int) ceil(($percentile / 100.0) * count($sortedValues)) - 1;
        $index = max(0, min(count($sortedValues) - 1, $index));

        return $sortedValues[$index];
    }
}
