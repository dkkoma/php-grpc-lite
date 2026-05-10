<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$streams = 10;
$messageCount = 100;
$payloadBytes = 100;
$sleepUs = 1000;
$nativeResponseMode = 'stream';
$implementation = 'php-grpc-lite';
$transport = 'native';

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--streams') {
        $streams = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--streams=')) {
        $streams = (int) substr($arg, strlen('--streams='));
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--sleep-us') {
        $sleepUs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--sleep-us=')) {
        $sleepUs = (int) substr($arg, strlen('--sleep-us='));
    } elseif ($arg === '--implementation') {
        $implementation = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--implementation=')) {
        $implementation = substr($arg, strlen('--implementation='));
    } elseif ($arg === '--native-response-mode') {
        $nativeResponseMode = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--native-response-mode=')) {
        $nativeResponseMode = substr($arg, strlen('--native-response-mode='));
    } elseif ($arg === '--transport') {
        ++$argIndex;
    } elseif (str_starts_with($arg, '--transport=')) {
        continue;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($streams <= 0 || $messageCount <= 0 || $payloadBytes < 0 || $sleepUs < 0) {
    usage('streams, message-count, payload-bytes, and sleep-us must be valid');
}
if (!is_file($autoload)) {
    throw new \RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$sample = ResourceSampler::measure(static function () use ($target, $nativeResponseMode, $implementation, $messageCount, $payloadBytes, $streams, $sleepUs): array {
    $options = [];
    if ($implementation === 'php-grpc-lite') {
        $options = [
            'php_grpc_lite.native_response_mode' => $nativeResponseMode,
        ];
    }

    $client = StreamingBenchHelper::client($target, $options);
    $request = StreamingBenchHelper::request($messageCount, $payloadBytes);
    $latencies = [];
    $firstYieldOffsets = [];
    $messages = 0;
    $maxMemoryUsage = memory_get_usage(false);
    $maxMemoryUsageReal = memory_get_usage(true);

    for ($stream = 0; $stream < $streams; $stream++) {
        $streamStartedNs = hrtime(true);
        $call = $client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            if ($count === 0) {
                $firstYieldOffsets[] = hrtime(true) - $streamStartedNs;
            }
            $count++;
            if ($sleepUs > 0) {
                usleep($sleepUs);
            }
            $maxMemoryUsage = max($maxMemoryUsage, memory_get_usage(false));
            $maxMemoryUsageReal = max($maxMemoryUsageReal, memory_get_usage(true));
        }
        $status = $call->getStatus();
        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("unexpected grpc status: {$status->code}: {$status->details}");
        }
        $latencies[] = hrtime(true) - $streamStartedNs;
        $messages += $count;
    }

    return [
        'latencies' => $latencies,
        'first_yield_offsets' => $firstYieldOffsets,
        'messages' => $messages,
        'memory_usage_max_bytes' => $maxMemoryUsage,
        'memory_usage_real_max_bytes' => $maxMemoryUsageReal,
    ];
});

$latencies = $sample['result']['latencies'];
$firstYieldOffsets = $sample['result']['first_yield_offsets'];
$messages = $sample['result']['messages'];
$elapsedNs = (int) $sample['metrics']['wall_time_ns_total']['value'];

$metrics = [
    'streams_total' => ['value' => $streams, 'unit' => 'streams'],
    'messages_total' => ['value' => $messages, 'unit' => 'messages'],
    'sleep_us_per_message' => ['value' => $sleepUs, 'unit' => 'us/message'],
    'wall_time_ns_total' => ['value' => $elapsedNs, 'unit' => 'ns'],
    'messages_per_second' => ['value' => $messages / ($elapsedNs / 1_000_000_000), 'unit' => 'messages/s'],
    'memory_usage_max_bytes' => ['value' => $sample['result']['memory_usage_max_bytes'], 'unit' => 'bytes'],
    'memory_usage_real_max_bytes' => ['value' => $sample['result']['memory_usage_real_max_bytes'], 'unit' => 'bytes'],
];
$metrics += $sample['metrics'];
foreach (UnaryBenchHelper::percentiles($latencies) as $name => $value) {
    $metrics['stream_latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}
foreach (UnaryBenchHelper::percentiles($firstYieldOffsets) as $name => $value) {
    $metrics['first_yield_offset_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}

printf(
    "implementation=%s transport=%s streams=%d messages=%d first-yield-p50=%.1fμs stream-p99=%.1fμs peak-delta=%.1fKiB wall=%.1fms\n",
    $implementation,
    'native',
    $streams,
    $messages,
    $metrics['first_yield_offset_p50_ns']['value'] / 1_000,
    $metrics['stream_latency_p99_ns']['value'] / 1_000,
    $metrics['memory_peak_delta_bytes']['value'] / 1024,
    $elapsedNs / 1_000_000,
);

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/slow-consumer-surface.php [--implementation=php-grpc-lite|ext-grpc] [--sleep-us=1000]\n");
    exit(2);
}
