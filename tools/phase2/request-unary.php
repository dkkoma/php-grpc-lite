<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\BenchTelemetry;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'request-unary';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 1.0;
$requestPayloadSizes = [0, 100, 1024, 10 * 1024, 100 * 1024];
$warmupCalls = 3;
$maxCalls = 0;
$diagnosticRpc = false;
$uploadReadCallback = false;
$nativeTransport = true;
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
    } elseif ($arg === '--diagnostic-rpc') {
        $diagnosticRpc = true;
    } elseif ($arg === '--upload-read-callback') {
        $uploadReadCallback = true;
    } elseif ($arg === '--native-transport') {
        $nativeTransport = true;
    } elseif ($arg === '--transport') {
        ++$argIndex;
    } elseif (str_starts_with($arg, '--transport=')) {
        continue;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($durationSec <= 0 || $requestPayloadSizes === [] || $warmupCalls < 0 || $maxCalls < 0) {
    usage('duration, payload-sizes, warmup-calls, and max-calls must be valid');
}
requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$client = UnaryBenchHelper::client($target);
foreach ($requestPayloadSizes as $requestPayloadBytes) {
    $requestPayload = $requestPayloadBytes > 0 ? str_repeat("\0", $requestPayloadBytes) : '';
    $request = UnaryBenchHelper::request(0, 0, $requestPayload);
    $benchTelemetry->setContext("request_unary_{$requestPayloadBytes}b", [
        'benchmark.target' => $target,
        'benchmark.duration_sec' => $durationSec,
        'benchmark.request_payload_bytes' => $requestPayloadBytes,
        'benchmark.response_payload_bytes' => 0,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.max_calls' => $maxCalls,
        'benchmark.upload_read_callback' => $uploadReadCallback,
        'benchmark.transport' => $transport,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($client, $request);
    }

    $latenciesNs = [];
    $diagnosticSeries = [];
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $maxCalls, $diagnosticRpc, $implementation, $requestPayloadBytes, $uploadReadCallback, $benchTelemetry, &$latenciesNs, &$diagnosticSeries): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            if ($diagnosticRpc) {
                $diagnostics = new \stdClass();
                $options = [];
                if ($implementation === 'php-grpc-lite') {
                    $options['php_grpc_lite.diagnostics'] = $diagnostics;
                    if ($uploadReadCallback) {
                        $options['php_grpc_lite.upload_read_callback'] = true;
                    }
                }
                $details = UnaryBenchHelper::callDetailed(
                    $client,
                    $request,
                    diagnosticMetadata(),
                    $options,
                );
                if ($implementation === 'php-grpc-lite') {
                    collectDiagnostics($diagnostics, $diagnosticSeries);
                }
                collectServerTiming($details['trailing_metadata'], $diagnosticSeries);
            } else {
                UnaryBenchHelper::call($client, $request);
            }
            $callEndNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchUnary', $callStartNs, $callEndNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchUnary',
                'benchmark.phase' => 'measurement',
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
    foreach (summarizeDiagnostics($diagnosticSeries) as $name => $metric) {
        $metrics[$name] = $metric;
    }
}

echo "OTEL spans exported.\n";

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


/**
 * @param array<string, list<int|float>> $series
 */
function collectDiagnostics(\stdClass $diagnostics, array &$series): void
{
    foreach (get_object_vars($diagnostics) as $name => $value) {
        if (!is_int($value) && !is_float($value)) {
            continue;
        }
        $series[$name][] = $value;
    }
}

/**
 * @param resource $handle
 * @return callable(int, string, int): void
 */
function diagnosticMetadata(): array
{
    return [
        'x-bench-server-timing' => ['1'],
        'x-bench-server-stats' => ['1'],
    ];
}

/**
 * @param array<string, list<string>> $trailers
 * @param array<string, list<int|float>> $series
 */
function collectServerTiming(array $trailers, array &$series): void
{
    foreach ([
        'x-bench-server-handler-ns' => 'server_handler_ns',
        'x-bench-server-payload-alloc-ns' => 'server_payload_alloc_ns',
        'x-bench-server-payload-bytes' => 'server_payload_bytes',
        'x-bench-server-request-payload-bytes' => 'server_request_payload_bytes',
        'x-bench-server-stats-handler-start-ns' => 'server_stats_handler_start_ns',
        'x-bench-server-stats-handler-end-ns' => 'server_stats_handler_end_ns',
        'x-bench-server-stats-in-payload-ns' => 'server_stats_in_payload_ns',
        'x-bench-server-stats-out-header-ns' => 'server_stats_out_header_ns',
        'x-bench-server-stats-out-payload-ns' => 'server_stats_out_payload_ns',
        'x-bench-server-stats-out-payload-bytes' => 'server_stats_out_payload_bytes',
        'x-bench-server-stats-out-payload-wire-bytes' => 'server_stats_out_payload_wire_bytes',
        'x-bench-server-stats-out-payload-compressed-bytes' => 'server_stats_out_payload_compressed_bytes',
    ] as $header => $metric) {
        $value = $trailers[$header][0] ?? null;
        if ($value === null || !is_numeric($value)) {
            continue;
        }
        $series[$metric][] = (int) $value;
    }
}

/**
 * @param array<string, list<int|float>> $series
 * @return array<string, array{value: int|float, unit: string}>
 */
function summarizeDiagnostics(array $series): array
{
    $metrics = [];
    foreach ($series as $name => $values) {
        if ($values === []) {
            continue;
        }
        $unit = diagnosticUnit($name);
        foreach (UnaryBenchHelper::percentiles($values) as $percentile => $value) {
            $metrics["diagnostic_rpc_{$name}_{$percentile}"] = [
                'value' => $value,
                'unit' => $unit,
            ];
        }
    }

    return $metrics;
}

function diagnosticUnit(string $name): string
{
    if (str_ends_with($name, '_ns') || str_ends_with($name, '_ns_total') || str_ends_with($name, '_ns_max')) {
        return 'ns';
    }
    if (str_ends_with($name, '_bytes') || str_ends_with($name, '_bytes_total') || str_ends_with($name, '_bytes_max')) {
        return 'bytes';
    }
    if (str_ends_with($name, '_total')) {
        return 'count';
    }

    return 'value';
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/request-unary.php --suite=request-unary --implementation=php-grpc-lite [--duration=1] [--request-payload-sizes=0,100,1024,10240,102400,1048576] [--warmup-calls=3] [--max-calls=0] [--diagnostic-rpc] [--upload-read-callback]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
