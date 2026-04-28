<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';
require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'metadata-header';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$calls = 50;
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
    } elseif ($arg === '--output') {
        $output = $args[++$argIndex] ?? null;
    } elseif (str_starts_with($arg, '--output=')) {
        $output = substr($arg, strlen('--output='));
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $output === null || $output === '') {
    usage('suite, implementation, target, and output are required');
}
if ($calls <= 0) {
    usage('calls must be greater than zero');
}

$client = UnaryBenchHelper::client($target);
$request = new BenchRequest();
$measurements = [];

foreach ($cases as [$requestKeys, $responseKeys, $valueBytes]) {
    $metadata = buildMetadata($requestKeys, $responseKeys, $valueBytes);
    $latenciesNs = [];
    $sample = ResourceSampler::measure(static function () use ($client, $request, $metadata, $calls, $responseKeys, &$latenciesNs): int {
        for ($callIndex = 0; $callIndex < $calls; $callIndex++) {
            $startedNs = hrtime(true);
            $call = $client->BenchUnary($request, $metadata);
            [, $status] = $call->wait();
            if ($status->code !== \Grpc\STATUS_OK) {
                throw new \RuntimeException("BenchUnary failed: {$status->details}");
            }
            $initialCount = countPrefix($call->getMetadata(), 'x-bench-initial-');
            $trailingCount = countPrefix($call->getTrailingMetadata(), 'x-bench-trailing-');
            if ($initialCount !== $responseKeys || $trailingCount !== $responseKeys) {
                throw new \RuntimeException("expected $responseKeys response metadata pairs, got $initialCount/$trailingCount");
            }
            $latenciesNs[] = hrtime(true) - $startedNs;
        }

        return $calls;
    });

    $metrics = $sample['metrics'];
    $elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
    $metrics['calls_total'] = ['value' => $sample['result'], 'unit' => 'calls'];
    $metrics['calls_per_second'] = ['value' => $sample['result'] / $elapsedSec, 'unit' => 'calls/s'];
    $metrics['wall_time_ns_per_call'] = ['value' => $metrics['wall_time_ns_total']['value'] / $sample['result'], 'unit' => 'ns/call'];
    foreach (UnaryBenchHelper::percentiles($latenciesNs) as $name => $value) {
        $metrics['latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
    }

    $measurements[] = ResultContract::measurement(
        sprintf('metadata_header_req_%d_resp_%d_value_%db', $requestKeys, $responseKeys, $valueBytes),
        'metadata-header',
        'BenchUnary',
        [
            'target' => $target,
            'calls' => $calls,
            'request_keys' => $requestKeys,
            'response_initial_keys' => $responseKeys,
            'response_trailing_keys' => $responseKeys,
            'value_bytes' => $valueBytes,
        ],
        $metrics,
    );
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-38s %10s %12s %12s\n", 'scenario', 'calls', 'p50', 'p99');
printf("%'-78s\n", '');
foreach ($measurements as $measurement) {
    printf("%-38s %10d %11.1fμs %11.1fμs\n", $measurement['name'], $measurement['metrics']['calls_total']['value'], $measurement['metrics']['latency_p50_ns']['value'] / 1_000, $measurement['metrics']['latency_p99_ns']['value'] / 1_000);
}
echo "JSON: $output\n";

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

function writeDocument(string $output, array $document): void
{
    $dir = dirname($output);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
    file_put_contents($output, ResultContract::encode($document));
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/metadata-header.php --suite=metadata-header --implementation=php-grpc-lite --output=var/bench-results/result.json [--calls=50]\n");
    exit(2);
}
