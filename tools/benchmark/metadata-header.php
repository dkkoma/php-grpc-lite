<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/RpcGap.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\ResourceSampler;
use PhpGrpcLite\Tools\Benchmark\RpcGap;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'metadata-header';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$calls = 50;
$rpcGapMs = RpcGap::fromEnvironment();
$cases = [
    [0, 0, 0],
    [10, 0, 32],
    [50, 0, 32],
    [10, 10, 32],
    [50, 50, 32],
];

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--suite') {
        $suite = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--suite=')) {
        $suite = substr($arg, strlen('--suite='));
    } elseif ($arg === '--implementation') {
        $implementation = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--implementation=')) {
        $implementation = substr($arg, strlen('--implementation='));
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif (RpcGap::consumeArgument($arg, $args, $argIndex, $rpcGapMs)) {
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($calls <= 0) {
    usage('calls must be greater than zero');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$client = UnaryBenchHelper::client($target);
$request = new BenchRequest();

foreach ($cases as [$requestKeys, $responseKeys, $valueBytes]) {
    $metadata = buildMetadata($requestKeys, $responseKeys, $valueBytes);
    $measurementName = sprintf('metadata_header_req_%d_resp_%d_value_%db', $requestKeys, $responseKeys, $valueBytes);
    $benchTelemetry->setContext($measurementName, [
        'benchmark.target' => $target,
        'benchmark.calls' => $calls,
        'benchmark.request_keys' => $requestKeys,
        'benchmark.response_initial_keys' => $responseKeys,
        'benchmark.response_trailing_keys' => $responseKeys,
        'benchmark.value_bytes' => $valueBytes,
        'benchmark.rpc_gap_ms' => $rpcGapMs,
    ]);
    $latenciesNs = [];
    $sample = ResourceSampler::measure(static function () use ($client, $request, $metadata, $calls, $responseKeys, $rpcGapMs, $benchTelemetry, &$latenciesNs): int {
        for ($callIndex = 0; $callIndex < $calls; $callIndex++) {
            $startedNs = hrtime(true);
            $details = UnaryBenchHelper::callDetailed(
                $client,
                $request,
                $metadata,
            );
            $callEndNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchUnary', $startedNs, $callEndNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchUnary',
            ]);
            $initialCount = countPrefix($details['metadata'], 'x-bench-initial-');
            $trailingCount = countPrefix($details['trailing_metadata'], 'x-bench-trailing-');
            if ($initialCount !== $responseKeys || $trailingCount !== $responseKeys) {
                throw new \RuntimeException("expected $responseKeys response metadata pairs, got $initialCount/$trailingCount");
            }
            $latenciesNs[] = $callEndNs - $startedNs;
            RpcGap::sleepBetweenCalls($rpcGapMs, $callIndex + 1 < $calls);
        }

        return $calls;
    });

}


echo "OTEL spans exported.\n";

/** @return array<string, list<string>> */
function buildMetadata(int $requestKeys, int $responseKeys, int $valueBytes): array
{
    $metadata = [
        'x-bench-response-metadata-count' => [(string) $responseKeys],
        'x-bench-response-metadata-value-bytes' => [(string) $valueBytes],
    ];
    $value = metadataValue($valueBytes);
    for ($index = 0; $index < $requestKeys; $index++) {
        $metadata[sprintf('x-bench-request-%03d', $index)] = [$value];
    }
    return $metadata;
}

function metadataValue(int $size): string
{
    if ($size <= 0) {
        return '';
    }
    return substr(str_repeat('abcdefghijklmnopqrstuvwxyz', intdiv($size + 25, 26)), 0, $size);
}

/** @param array<string, list<string>> $metadata */
function countPrefix(array $metadata, string $prefix): int
{
    $count = 0;
    foreach (array_keys($metadata) as $key) {
        if (str_starts_with($key, $prefix)) {
            $count++;
        }
    }
    return $count;
}


function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/metadata-header.php --suite=metadata-header --implementation=php-grpc-lite [--calls=50] [--rpc-gap-ms=0]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
