<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'streaming-diagnostic';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$streams = 1000;
$warmupStreams = 3;
$messageCount = 10;
$payloadBytes = 100 * 1024;
$nativeTransport = true;
$nativeResponseMode = 'direct';
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
    } elseif ($arg === '--streams') {
        $streams = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--streams=')) {
        $streams = (int) substr($arg, strlen('--streams='));
    } elseif ($arg === '--warmup-streams') {
        $warmupStreams = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-streams=')) {
        $warmupStreams = (int) substr($arg, strlen('--warmup-streams='));
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--native-transport') {
        $nativeTransport = true;
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

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}
if ($streams <= 0 || $warmupStreams < 0 || $messageCount <= 0 || $payloadBytes < 0) {
    usage('streams, warmup-streams, message-count, and payload-bytes must be valid');
}

requireAutoload($autoload);

$clientOptions = [
    'php_grpc_lite.native_response_mode' => $nativeResponseMode,
];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = StreamingBenchHelper::client($target, $clientOptions);
$request = StreamingBenchHelper::request($messageCount, $payloadBytes);
for ($warmup = 0; $warmup < $warmupStreams; $warmup++) {
    StreamingBenchHelper::drain($client, $request);
}
$streamLatenciesNs = [];
$series = [
    'server_stats_in_payload_ns' => [],
    'server_stats_out_header_ns' => [],
    'server_stats_first_out_payload_ns' => [],
    'server_stats_last_out_payload_ns' => [],
    'server_stats_out_payload_count' => [],
    'server_stats_out_payload_bytes' => [],
    'server_stats_out_payload_wire_bytes' => [],
];

$sample = ResourceSampler::measure(static function () use ($client, $request, $streams, &$streamLatenciesNs, &$series): int {
    $messages = 0;
    for ($stream = 0; $stream < $streams; $stream++) {
        $streamStartNs = hrtime(true);
        $call = $client->BenchServerStream($request, ['x-bench-server-stats' => ['1']]);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }
        $status = $call->getStatus();
        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("unexpected grpc status: {$status->code}");
        }
        if ($count !== $request->getMessageCount()) {
            throw new \RuntimeException("expected {$request->getMessageCount()} messages, got $count");
        }
        $streamLatenciesNs[] = hrtime(true) - $streamStartNs;
        collectTrailerSeries($call->getTrailingMetadata(), $series);
        $messages += $count;
    }

    return $messages;
});

$messages = $sample['result'];
$metrics = $sample['metrics'];
$elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
$metrics['streams_total'] = ['value' => $streams, 'unit' => 'streams'];
$metrics['messages_total'] = ['value' => $messages, 'unit' => 'messages'];
$metrics['messages_per_second'] = ['value' => $messages / $elapsedSec, 'unit' => 'messages/s'];
$metrics['wall_time_ns_per_message'] = ['value' => $metrics['wall_time_ns_total']['value'] / $messages, 'unit' => 'ns/message'];
foreach (UnaryBenchHelper::percentiles($streamLatenciesNs) as $name => $value) {
    $metrics['stream_latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}
foreach (summarizeSeries($series) as $name => $summary) {
    foreach ($summary as $percentile => $value) {
        $metrics[$name . '_' . $percentile] = ['value' => $value, 'unit' => str_ends_with($name, '_count') ? 'count' : 'ns'];
    }
}

$measurement = ResultContract::measurement('streaming_diagnostic', 'streaming-diagnostic', 'BenchServerStream', [
    'target' => $target,
    'streams' => $streams,
    'warmup_streams' => $warmupStreams,
    'message_count' => $messageCount,
    'payload_bytes' => $payloadBytes,
    'native_transport' => $nativeTransport,
    'native_response_mode' => $nativeResponseMode,
    'transport' => $transport,
], $metrics);

$document = ResultContract::document($suite, $implementation, [$measurement]);
writeDocument($output, $document);

printf("%-24s %12s %12s %12s %12s\n", 'scenario', 'messages', 'msg/s', 'p50 stream', 'p99 stream');
printf("%'-76s\n", '');
printf("%-24s %12d %12.1f %11.1fμs %11.1fμs\n", $measurement['name'], $messages, $metrics['messages_per_second']['value'], $metrics['stream_latency_p50_ns']['value'] / 1_000, $metrics['stream_latency_p99_ns']['value'] / 1_000);
echo "JSON: $output\n";

/** @param array<string, list<string>> $trailers */
function collectTrailerSeries(array $trailers, array &$series): void
{
    foreach ($series as $name => $_values) {
        $header = 'x-bench-' . str_replace('_', '-', $name);
        $value = $trailers[$header][0] ?? null;
        if ($value === null) {
            continue;
        }
        $series[$name][] = (int) $value;
    }
}

/** @param array<string, list<int>> $series */
function summarizeSeries(array $series): array
{
    $summaries = [];
    foreach ($series as $name => $values) {
        if ($values === []) {
            continue;
        }
        sort($values);
        $summaries[$name] = UnaryBenchHelper::percentiles($values);
    }
    return $summaries;
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
    fwrite(STDERR, "Usage: php tools/phase2/streaming-diagnostic.php --suite=streaming-diagnostic --implementation=php-grpc-lite --output=var/bench-results/result.json [--streams=1000] [--warmup-streams=3] [--message-count=10] [--payload-bytes=102400]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
