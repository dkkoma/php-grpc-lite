<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$streams = 10;
$messageCount = 100;
$payloadBytes = 100;
$sleepUs = 1000;
$transport = 'curl';
$nativeResponseMode = 'direct';

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--output') {
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
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } elseif ($arg === '--native-response-mode') {
        $nativeResponseMode = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--native-response-mode=')) {
        $nativeResponseMode = substr($arg, strlen('--native-response-mode='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($output === null || $output === '' || $streams <= 0 || $messageCount <= 0 || $payloadBytes < 0 || $sleepUs < 0) {
    usage('output, streams, message-count, payload-bytes, and sleep-us must be valid');
}
if (!is_file($autoload)) {
    throw new \RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$client = StreamingBenchHelper::client($target, [
    'php_grpc_lite.transport' => $transport,
    'php_grpc_lite.native_transport' => $transport === 'native',
    'php_grpc_lite.native_response_mode' => $nativeResponseMode,
]);
$request = StreamingBenchHelper::request($messageCount, $payloadBytes);
$latencies = [];
$firstYieldOffsets = [];
$messages = 0;

$startedNs = hrtime(true);
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
    }
    $status = $call->getStatus();
    if ($status->code !== \Grpc\STATUS_OK) {
        throw new \RuntimeException("unexpected grpc status: {$status->code}: {$status->details}");
    }
    $latencies[] = hrtime(true) - $streamStartedNs;
    $messages += $count;
}
$elapsedNs = hrtime(true) - $startedNs;

$metrics = [
    'streams_total' => ['value' => $streams, 'unit' => 'streams'],
    'messages_total' => ['value' => $messages, 'unit' => 'messages'],
    'sleep_us_per_message' => ['value' => $sleepUs, 'unit' => 'us/message'],
    'wall_time_ns_total' => ['value' => $elapsedNs, 'unit' => 'ns'],
    'messages_per_second' => ['value' => $messages / ($elapsedNs / 1_000_000_000), 'unit' => 'messages/s'],
];
foreach (UnaryBenchHelper::percentiles($latencies) as $name => $value) {
    $metrics['stream_latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}
foreach (UnaryBenchHelper::percentiles($firstYieldOffsets) as $name => $value) {
    $metrics['first_yield_offset_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}

$document = ResultContract::document('native-slow-consumer', 'php-grpc-lite', [
    ResultContract::measurement('slow_consumer_surface', 'native-slow-consumer', 'BenchServerStream', [
        'target' => $target,
        'transport' => $transport,
        'native_response_mode' => $nativeResponseMode,
        'streams' => $streams,
        'message_count' => $messageCount,
        'payload_bytes' => $payloadBytes,
        'sleep_us' => $sleepUs,
        'note' => $transport === 'native'
            ? 'native MVP drains the batch before yielding; this is a limitation check'
            : 'curl surface incremental reference',
    ], $metrics),
]);

$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, ResultContract::encode($document));
printf(
    "transport=%s streams=%d messages=%d first-yield-p50=%.1fμs stream-p99=%.1fμs\n",
    $transport,
    $streams,
    $messages,
    $metrics['first_yield_offset_p50_ns']['value'] / 1_000,
    $metrics['stream_latency_p99_ns']['value'] / 1_000,
);

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/slow-consumer-surface.php --output=var/bench-results/result.json [--transport=curl|native] [--sleep-us=1000]\n");
    exit(2);
}
