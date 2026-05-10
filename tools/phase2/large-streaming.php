<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/StreamingBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\BenchTelemetry;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'large-streaming';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$messageCounts = [10_000, 100_000];
$payloadBytes = 100;

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
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--message-counts') {
        $messageCounts = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--message-counts=')) {
        $messageCounts = parseIntList(substr($arg, strlen('--message-counts=')));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}
if ($messageCounts === [] || $payloadBytes < 0) {
    usage('message-counts and payload-bytes must be valid');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::fromEnvironment($suite, $implementation);
if ($benchTelemetry !== null) {
    register_shutdown_function([$benchTelemetry, 'shutdown']);
}

$client = StreamingBenchHelper::client($target);
$measurements = [];
foreach ($messageCounts as $messageCount) {
    $request = StreamingBenchHelper::request($messageCount, $payloadBytes);
    $benchTelemetry?->setContext("large_streaming_count_$messageCount", [
        'benchmark.target' => $target,
        'benchmark.message_count' => $messageCount,
        'benchmark.payload_bytes' => $payloadBytes,
    ]);
    $sample = ResourceSampler::measure(static fn (): int => StreamingBenchHelper::drain($client, $request));
    $metrics = $sample['metrics'];
    $metrics['messages_total'] = ['value' => $sample['result'], 'unit' => 'messages'];
    $metrics['messages_per_second'] = ['value' => $sample['result'] / ($metrics['wall_time_ns_total']['value'] / 1_000_000_000), 'unit' => 'messages/s'];
    $metrics['wall_time_ns_per_message'] = ['value' => $metrics['wall_time_ns_total']['value'] / $sample['result'], 'unit' => 'ns/message'];

    $measurements[] = ResultContract::measurement("large_streaming_count_$messageCount", 'large-streaming', 'BenchServerStream', [
        'target' => $target,
        'message_count' => $messageCount,
        'payload_bytes' => $payloadBytes,
    ], $metrics);
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-30s %12s %12s %12s\n", 'scenario', 'messages', 'elapsed', 'msg/s');
printf("%'-72s\n", '');
foreach ($measurements as $measurement) {
    printf("%-30s %12d %11.1fms %12.1f\n", $measurement['name'], $measurement['metrics']['messages_total']['value'], $measurement['metrics']['wall_time_ns_total']['value'] / 1_000_000, $measurement['metrics']['messages_per_second']['value']);
}
echo "JSON: $output\n";

/** @return list<int> */
function parseIntList(string $value): array
{
    $items = [];
    foreach (explode(',', $value) as $part) {
        $number = (int) trim($part);
        if ($number > 0) {
            $items[] = $number;
        }
    }
    return $items;
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
    fwrite(STDERR, "Usage: php tools/phase2/large-streaming.php --suite=large-streaming --implementation=php-grpc-lite --output=var/bench-results/result.json [--message-counts=10000,100000] [--payload-bytes=100]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
