<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\BenchTelemetry;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'throughput-streaming';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 3.0;
$messageCount = 1000;
$payloadBytes = 100;
$serverDelayMs = 0;
$warmupStreams = 1;
$transport = 'native';

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
    } elseif ($arg === '--duration') {
        $durationSec = (float) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--duration=')) {
        $durationSec = (float) substr($arg, strlen('--duration='));
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--server-delay-ms') {
        $serverDelayMs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--server-delay-ms=')) {
        $serverDelayMs = (int) substr($arg, strlen('--server-delay-ms='));
    } elseif ($arg === '--warmup-streams') {
        $warmupStreams = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-streams=')) {
        $warmupStreams = (int) substr($arg, strlen('--warmup-streams='));
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}
if ($durationSec <= 0 || $messageCount <= 0 || $payloadBytes < 0 || $serverDelayMs < 0 || $warmupStreams < 0) {
    usage('duration, message-count, payload-bytes, server-delay-ms, and warmup-streams must be valid');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::fromEnvironment($suite, $implementation);
if ($benchTelemetry !== null) {
    register_shutdown_function([$benchTelemetry, 'shutdown']);
}

$clientOptions = [];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = StreamingBenchHelper::client($target, $clientOptions);
$request = StreamingBenchHelper::request($messageCount, $payloadBytes, $serverDelayMs);
$benchTelemetry?->setContext('throughput_streaming', [
    'benchmark.target' => $target,
    'benchmark.duration_sec' => $durationSec,
    'benchmark.message_count' => $messageCount,
    'benchmark.payload_bytes' => $payloadBytes,
    'benchmark.server_delay_ms' => $serverDelayMs,
    'benchmark.warmup_streams' => $warmupStreams,
    'benchmark.transport' => $transport,
]);
for ($warmup = 0; $warmup < $warmupStreams; $warmup++) {
    StreamingBenchHelper::drain($client, $request);
}

$streamLatenciesNs = [];
$deadlineNs = (int) round($durationSec * 1_000_000_000);
$sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $benchTelemetry, &$streamLatenciesNs): int {
    $startedNs = hrtime(true);
    $messages = 0;

    do {
        $streamStartNs = hrtime(true);
        $messages += StreamingBenchHelper::drainWithTelemetry($benchTelemetry, $client, $request, ['benchmark.phase' => 'measurement']);
        $streamLatenciesNs[] = hrtime(true) - $streamStartNs;
    } while (hrtime(true) - $startedNs < $deadlineNs);

    return $messages;
});

$messages = $sample['result'];
$streams = count($streamLatenciesNs);
$metrics = $sample['metrics'];
$elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
$percentiles = UnaryBenchHelper::percentiles($streamLatenciesNs);
$metrics['streams_total'] = ['value' => $streams, 'unit' => 'streams'];
$metrics['messages_total'] = ['value' => $messages, 'unit' => 'messages'];
$metrics['streams_per_second'] = ['value' => $streams / $elapsedSec, 'unit' => 'streams/s'];
$metrics['messages_per_second'] = ['value' => $messages / $elapsedSec, 'unit' => 'messages/s'];
$metrics['wall_time_ns_per_message'] = ['value' => $metrics['wall_time_ns_total']['value'] / $messages, 'unit' => 'ns/message'];
foreach ($percentiles as $name => $value) {
    $metrics['stream_latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}

$document = ResultContract::document($suite, $implementation, [
    ResultContract::measurement('throughput_streaming', 'throughput', 'BenchServerStream', [
        'target' => $target,
        'duration_sec' => $durationSec,
        'message_count' => $messageCount,
        'payload_bytes' => $payloadBytes,
        'server_delay_ms' => $serverDelayMs,
        'concurrency' => 1,
        'transport' => $transport,
    ], $metrics),
]);

writeDocument($output, $document);

printf("%-24s %8s %12s %12s %12s\n", 'scenario', 'streams', 'msg/s', 'p50 stream', 'p99 stream');
printf("%'-76s\n", '');
printf("%-24s %8d %12.1f %11.1fμs %11.1fμs\n", 'throughput_streaming', $streams, $metrics['messages_per_second']['value'], $metrics['stream_latency_p50_ns']['value'] / 1_000, $metrics['stream_latency_p99_ns']['value'] / 1_000);
echo "JSON: $output\n";

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
    fwrite(STDERR, "Usage: php tools/phase2/throughput-streaming.php --suite=throughput-streaming --implementation=php-grpc-lite --output=var/bench-results/result.json [--duration=3] [--message-count=1000] [--payload-bytes=100]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
