<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/StreamingBenchHelper.php';

use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\ResourceSampler;
use PhpGrpcLite\Tools\Benchmark\StreamingBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'large-streaming';
$implementation = 'php-grpc-lite';
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

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($messageCounts === [] || $payloadBytes < 0) {
    usage('message-counts and payload-bytes must be valid');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$client = StreamingBenchHelper::client($target);
foreach ($messageCounts as $messageCount) {
    $request = StreamingBenchHelper::request($messageCount, $payloadBytes);
    $benchTelemetry->setContext("large_streaming_count_$messageCount", [
        'benchmark.target' => $target,
        'benchmark.message_count' => $messageCount,
        'benchmark.payload_bytes' => $payloadBytes,
    ]);
    $sample = ResourceSampler::measure(static function () use ($benchTelemetry, $client, $request): int {
        $streamStartNs = hrtime(true);
        $messages = StreamingBenchHelper::drain($client, $request);
        $streamEndNs = hrtime(true);
        $benchTelemetry->recordRpcSpan('BenchServerStream', $streamStartNs, $streamEndNs, [
            'rpc.service' => 'helloworld.Greeter',
            'rpc.method' => 'BenchServerStream',
            'benchmark.phase' => 'measurement',
        ]);
        return $messages;
    });
    $metrics = $sample['metrics'];
    $metrics['messages_total'] = ['value' => $sample['result'], 'unit' => 'messages'];
    $metrics['messages_per_second'] = ['value' => $sample['result'] / ($metrics['wall_time_ns_total']['value'] / 1_000_000_000), 'unit' => 'messages/s'];
    $metrics['wall_time_ns_per_message'] = ['value' => $metrics['wall_time_ns_total']['value'] / $sample['result'], 'unit' => 'ns/message'];
}


echo "OTEL spans exported.\n";

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


function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/large-streaming.php --suite=large-streaming --implementation=php-grpc-lite [--message-counts=10000,100000] [--payload-bytes=100]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
