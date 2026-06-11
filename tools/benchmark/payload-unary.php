<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Grpc\ChannelCredentials;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\ResourceSampler;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'payload-unary';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 1.0;
$payloadSizes = null;
$requestPayloadSizes = null;
$warmupCalls = 3;
$maxCalls = 0;
$transport = 'native';
$tlsRoot = '';

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
    } elseif ($arg === '--duration') {
        $durationSec = (float) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--duration=')) {
        $durationSec = (float) substr($arg, strlen('--duration='));
    } elseif ($arg === '--payload-sizes') {
        $payloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--payload-sizes=')) {
        $payloadSizes = parseIntList(substr($arg, strlen('--payload-sizes=')));
    } elseif ($arg === '--request-payload-sizes') {
        $requestPayloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--request-payload-sizes=')) {
        $requestPayloadSizes = parseIntList(substr($arg, strlen('--request-payload-sizes=')));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif ($arg === '--max-calls') {
        $maxCalls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--max-calls=')) {
        $maxCalls = (int) substr($arg, strlen('--max-calls='));
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } elseif ($arg === '--tls-root') {
        $tlsRoot = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--tls-root=')) {
        $tlsRoot = substr($arg, strlen('--tls-root='));
    } else {
        usage("unexpected argument: $arg");
    }
}

/*
 * Suite-aware defaults:
 *   payload-unary / tls-payload-unary: response payload sweep (download path)
 *   upload-unary / tls-upload-unary:   request payload sweep (upload path)
 */
$isUploadSuite = str_contains($suite, 'upload-unary');
if ($payloadSizes === null) {
    $payloadSizes = $isUploadSuite ? [] : [0, 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024, 4 * 1024 * 1024];
}
if ($requestPayloadSizes === null) {
    $requestPayloadSizes = $isUploadSuite ? [1024, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024] : [];
}
if (str_starts_with($suite, 'tls-') && $target === 'test-server:50051') {
    $target = 'test-server:50052';
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($durationSec <= 0 || ($payloadSizes === [] && $requestPayloadSizes === []) || $warmupCalls < 0 || $maxCalls < 0) {
    usage('duration, warmup-calls, and max-calls must be valid, and at least one of payload-sizes / request-payload-sizes must be non-empty');
}
requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$clientOptions = tlsClientOptions($suite, $tlsRoot);
$client = UnaryBenchHelper::client($target, $clientOptions);

$cases = [];
foreach ($payloadSizes as $payloadBytes) {
    $cases[] = [
        'name' => "payload_unary_{$payloadBytes}b",
        'request' => UnaryBenchHelper::request($payloadBytes),
        'attributes' => [
            'benchmark.payload_bytes' => $payloadBytes,
            'benchmark.request_payload_bytes' => 0,
            'benchmark.direction' => 'response',
        ],
    ];
}
foreach ($requestPayloadSizes as $requestPayloadBytes) {
    $cases[] = [
        'name' => "upload_unary_{$requestPayloadBytes}b",
        'request' => UnaryBenchHelper::request(0, 0, str_repeat('x', $requestPayloadBytes)),
        'attributes' => [
            'benchmark.payload_bytes' => 0,
            'benchmark.request_payload_bytes' => $requestPayloadBytes,
            'benchmark.direction' => 'request',
        ],
    ];
}

foreach ($cases as $case) {
    $request = $case['request'];
    $benchTelemetry->setContext($case['name'], $case['attributes'] + [
        'benchmark.target' => $target,
        'benchmark.duration_sec' => $durationSec,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.max_calls' => $maxCalls,
        'benchmark.transport' => $transport,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($client, $request);
    }

    $latenciesNs = [];
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $maxCalls, $benchTelemetry, &$latenciesNs): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            UnaryBenchHelper::call($client, $request);
            $callEndNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchUnary', $callStartNs, $callEndNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchUnary',
            ]);
            $latenciesNs[] = $callEndNs - $callStartNs;
            $calls++;
        } while (hrtime(true) - $startedNs < $deadlineNs && ($maxCalls === 0 || $calls < $maxCalls));
        return $calls;
    });

    $calls = $sample['result'];
    $metrics = $sample['metrics'];
    $elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
    $metrics['calls_total'] = ['value' => $calls, 'unit' => 'calls'];
    $metrics['calls_per_second'] = ['value' => $calls / $elapsedSec, 'unit' => 'calls/s'];
    $metrics['wall_time_ns_per_call'] = ['value' => $metrics['wall_time_ns_total']['value'] / $calls, 'unit' => 'ns/call'];
    foreach (UnaryBenchHelper::percentiles($latenciesNs) as $name => $value) {
        $metrics['latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
    }
}

echo "OTEL spans exported.\n";

/** @return list<int> */
function parseIntList(string $value): array
{
    $items = [];
    foreach (explode(',', $value) as $part) {
        if (trim($part) === '') {
            continue;
        }
        $number = (int) trim($part);
        if ($number >= 0) {
            $items[] = $number;
        }
    }
    return $items;
}

/** @return array<string, mixed> */
function tlsClientOptions(string $suite, string $tlsRoot): array
{
    if (!str_starts_with($suite, 'tls-')) {
        return [];
    }
    if ($tlsRoot === '') {
        $tlsRoot = dirname(__DIR__, 2) . '/poc/test-server/certs/server.crt';
    }
    $root = file_get_contents($tlsRoot);
    if ($root === false) {
        throw new RuntimeException("TLS root certificate not found: $tlsRoot");
    }
    return ['credentials' => ChannelCredentials::createSsl($root)];
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/payload-unary.php --suite=payload-unary|tls-payload-unary|upload-unary|tls-upload-unary --implementation=php-grpc-lite [--duration=1] [--payload-sizes=0,100,1024,10240,102400,1048576,4194304] [--request-payload-sizes=1024,16384,65536,262144,1048576,4194304] [--warmup-calls=3] [--max-calls=0] [--tls-root=...]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
