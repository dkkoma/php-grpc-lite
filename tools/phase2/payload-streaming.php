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

$suite = 'payload-streaming';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$streams = 10;
$messageCount = 100;
$payloadSizes = [0, 100, 1024, 10 * 1024];
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
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-sizes') {
        $payloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--payload-sizes=')) {
        $payloadSizes = parseIntList(substr($arg, strlen('--payload-sizes=')));
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
if ($streams <= 0 || $messageCount <= 0 || $payloadSizes === []) {
    usage('streams, message-count, and payload-sizes must be valid');
}

requireAutoload($autoload);

$clientOptions = [];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = StreamingBenchHelper::client($target, $clientOptions);
$measurements = [];
foreach ($payloadSizes as $payloadBytes) {
    $request = StreamingBenchHelper::request($messageCount, $payloadBytes);
    $streamLatenciesNs = [];
    $sample = ResourceSampler::measure(static function () use ($client, $request, $streams, &$streamLatenciesNs): int {
        $messages = 0;
        for ($stream = 0; $stream < $streams; $stream++) {
            $streamStartNs = hrtime(true);
            $messages += StreamingBenchHelper::drain($client, $request);
            $streamLatenciesNs[] = hrtime(true) - $streamStartNs;
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

    $measurements[] = ResultContract::measurement("payload_streaming_{$payloadBytes}b", 'payload-streaming', 'BenchServerStream', [
        'target' => $target,
        'streams' => $streams,
        'message_count' => $messageCount,
        'payload_bytes' => $payloadBytes,
        'transport' => $transport,
    ], $metrics);
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-26s %12s %12s %12s\n", 'scenario', 'messages', 'msg/s', 'p50 stream');
printf("%'-72s\n", '');
foreach ($measurements as $measurement) {
    printf("%-26s %12d %12.1f %11.1fμs\n", $measurement['name'], $measurement['metrics']['messages_total']['value'], $measurement['metrics']['messages_per_second']['value'], $measurement['metrics']['stream_latency_p50_ns']['value'] / 1_000);
}
echo "JSON: $output\n";

/** @return list<int> */
function parseIntList(string $value): array
{
    $items = [];
    foreach (explode(',', $value) as $part) {
        $number = (int) trim($part);
        if ($number >= 0) {
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
    fwrite(STDERR, "Usage: php tools/phase2/payload-streaming.php --suite=payload-streaming --implementation=php-grpc-lite --output=var/bench-results/result.json [--streams=10] [--message-count=100] [--payload-sizes=0,100,1024,10240]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
