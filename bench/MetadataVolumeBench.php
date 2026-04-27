<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * Unary metadata overhead for request headers and response initial/trailing
 * metadata. Binary metadata is intentionally kept out until compatibility is
 * nailed down separately.
 */
#[Bench\BeforeMethods('setUp')]
final class MetadataVolumeBench
{
    private GreeterClient $client;

    public function setUp(): void
    {
        $this->client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    /** @return iterable<string, array{request_keys: int, response_keys: int, value_bytes: int}> */
    public function provideMetadataCases(): iterable
    {
        yield 'req_0_resp_0' => [
            'request_keys' => 0,
            'response_keys' => 0,
            'value_bytes' => 0,
        ];
        yield 'req_10_resp_0_value_32b' => [
            'request_keys' => 10,
            'response_keys' => 0,
            'value_bytes' => 32,
        ];
        yield 'req_50_resp_0_value_32b' => [
            'request_keys' => 50,
            'response_keys' => 0,
            'value_bytes' => 32,
        ];
        yield 'req_10_resp_10_value_32b' => [
            'request_keys' => 10,
            'response_keys' => 10,
            'value_bytes' => 32,
        ];
        yield 'req_50_resp_50_value_32b' => [
            'request_keys' => 50,
            'response_keys' => 50,
            'value_bytes' => 32,
        ];
    }

    #[Bench\ParamProviders('provideMetadataCases')]
    #[Bench\Revs(50), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchUnaryMetadataVolume(array $params): void
    {
        $req = new BenchRequest();
        $metadata = $this->buildRequestMetadata(
            $params['request_keys'],
            $params['response_keys'],
            $params['value_bytes'],
        );

        $call = $this->client->BenchUnary($req, $metadata);
        [, $status] = $call->wait();
        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("unexpected status {$status->code}: {$status->details}");
        }

        $initialCount = $this->countPrefix($call->getMetadata(), 'x-bench-initial-');
        $trailingCount = $this->countPrefix($call->getTrailingMetadata(), 'x-bench-trailing-');
        if ($initialCount !== $params['response_keys'] || $trailingCount !== $params['response_keys']) {
            throw new \RuntimeException(
                "expected {$params['response_keys']} response metadata pairs, got $initialCount/$trailingCount"
            );
        }
    }

    /** @return array<string, list<string>> */
    private function buildRequestMetadata(int $requestKeys, int $responseKeys, int $valueBytes): array
    {
        $metadata = [
            'x-bench-response-metadata-count' => [(string) $responseKeys],
            'x-bench-response-metadata-value-bytes' => [(string) $valueBytes],
        ];
        $value = $this->metadataValue($valueBytes);
        for ($i = 0; $i < $requestKeys; $i++) {
            $metadata[sprintf('x-bench-request-%03d', $i)] = [$value];
        }
        return $metadata;
    }

    private function metadataValue(int $size): string
    {
        if ($size <= 0) {
            return '';
        }
        return substr(str_repeat('abcdefghijklmnopqrstuvwxyz', intdiv($size + 25, 26)), 0, $size);
    }

    /** @param array<string, list<string>> $metadata */
    private function countPrefix(array $metadata, string $prefix): int
    {
        $count = 0;
        foreach (array_keys($metadata) as $key) {
            if (str_starts_with($key, $prefix)) {
                $count++;
            }
        }
        return $count;
    }
}
