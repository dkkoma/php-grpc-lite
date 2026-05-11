<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Benchmark;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

final class UnaryBenchHelper
{
    /** @param array<string, mixed> $options */
    public static function client(string $target, array $options = []): GreeterClient
    {
        return new GreeterClient($target, $options + [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public static function request(int $payloadBytes = 100, int $serverDelayMs = 0, string $requestPayload = ''): BenchRequest
    {
        $request = new BenchRequest();
        $request->setPayloadBytes($payloadBytes);
        $request->setServerDelayMs($serverDelayMs);
        if ($requestPayload !== '') {
            $request->setRequestPayload($requestPayload);
        }

        return $request;
    }

    /** @param array<string, mixed> $options */
    public static function call(GreeterClient $client, BenchRequest $request, array $options = []): void
    {
        self::callDetailed($client, $request, [], $options);
    }

    /**
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     * @return array{metadata: array<string, list<string>>, trailing_metadata: array<string, list<string>>}
     */
    public static function callDetailed(
        GreeterClient $client,
        BenchRequest $request,
        array $metadata = [],
        array $options = [],
    ): array {
        $call = $client->BenchUnary($request, $metadata, $options);
        [$response, $status] = $call->wait();
        if ($status->code !== \Grpc\STATUS_OK || $response === null) {
            throw new \RuntimeException("BenchUnary failed: {$status->details}");
        }

        return [
            'metadata' => $call->getMetadata(),
            'trailing_metadata' => $call->getTrailingMetadata(),
        ];
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
