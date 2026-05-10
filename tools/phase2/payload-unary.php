<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\BenchTelemetry;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'payload-unary';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 1.0;
$payloadSizes = [0, 100, 1024, 10 * 1024, 100 * 1024];
$warmupCalls = 3;
$maxCalls = 0;
$diagnosticRpc = false;
$serverCachedPayload = false;
$curlTraceOutput = null;
$curlTraceCalls = 0;
$returnTransferFastPath = false;
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
    } elseif ($arg === '--payload-sizes') {
        $payloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--payload-sizes=')) {
        $payloadSizes = parseIntList(substr($arg, strlen('--payload-sizes=')));
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
    } elseif ($arg === '--server-cached-payload') {
        $serverCachedPayload = true;
    } elseif ($arg === '--return-transfer-fast-path') {
        $returnTransferFastPath = true;
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } elseif ($arg === '--curl-trace-output') {
        $curlTraceOutput = $args[++$argIndex] ?? null;
    } elseif (str_starts_with($arg, '--curl-trace-output=')) {
        $curlTraceOutput = substr($arg, strlen('--curl-trace-output='));
    } elseif ($arg === '--curl-trace-calls') {
        $curlTraceCalls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--curl-trace-calls=')) {
        $curlTraceCalls = (int) substr($arg, strlen('--curl-trace-calls='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}
if ($durationSec <= 0 || $payloadSizes === [] || $warmupCalls < 0 || $maxCalls < 0) {
    usage('duration, payload-sizes, warmup-calls, and max-calls must be valid');
}
if ($curlTraceCalls < 0) {
    usage('curl-trace-calls must be valid');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::fromEnvironment($suite, $implementation);
if ($benchTelemetry !== null) {
    register_shutdown_function([$benchTelemetry, 'shutdown']);
}

$curlTraceHandle = null;
if ($curlTraceOutput !== null && $curlTraceOutput !== '') {
    $traceDir = dirname($curlTraceOutput);
    if (!is_dir($traceDir)) {
        mkdir($traceDir, 0777, true);
    }
    $curlTraceHandle = fopen($curlTraceOutput, 'wb');
    if ($curlTraceHandle === false) {
        throw new \RuntimeException("failed to open curl trace output: $curlTraceOutput");
    }
    fwrite($curlTraceHandle, "# curl debug trace; timing is relative to call option creation; latency is diagnostic only\n");
}

$clientOptions = [];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = UnaryBenchHelper::client($target, $clientOptions);
$measurements = [];
foreach ($payloadSizes as $payloadBytes) {
    $request = UnaryBenchHelper::request($payloadBytes);
    $benchTelemetry?->setContext("payload_unary_{$payloadBytes}b", [
        'benchmark.target' => $target,
        'benchmark.duration_sec' => $durationSec,
        'benchmark.payload_bytes' => $payloadBytes,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.max_calls' => $maxCalls,
        'benchmark.server_cached_payload' => $serverCachedPayload,
        'benchmark.return_transfer_fast_path' => $returnTransferFastPath,
        'benchmark.transport' => $transport,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($client, $request);
    }

    $latenciesNs = [];
    $diagnosticSeries = [];
    $curlTraceWritten = 0;
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $maxCalls, $diagnosticRpc, $implementation, $serverCachedPayload, $returnTransferFastPath, $payloadBytes, $curlTraceHandle, $curlTraceCalls, $benchTelemetry, &$latenciesNs, &$diagnosticSeries, &$curlTraceWritten): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            if ($diagnosticRpc) {
                $diagnostics = new \stdClass();
                $options = [];
                if ($implementation === 'php-grpc-lite') {
                    $options['php_grpc_lite.diagnostics'] = $diagnostics;
                    if ($returnTransferFastPath) {
                        $options['php_grpc_lite.return_transfer_fast_path'] = true;
                    }
                }
                if ($implementation === 'php-grpc-lite' && $curlTraceHandle !== null && $curlTraceWritten < $curlTraceCalls) {
                    $curlTraceWritten++;
                    $options['php_grpc_lite.curl_trace'] = curlTraceCallback($curlTraceHandle, $payloadBytes, $curlTraceWritten);
                }
                $details = UnaryBenchHelper::callDetailed(
                    $client,
                    $request,
                    diagnosticMetadata($serverCachedPayload),
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
            $benchTelemetry?->recordRpcSpan('BenchUnary', $callStartNs, $callEndNs, [
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

    $measurements[] = ResultContract::measurement("payload_unary_{$payloadBytes}b", 'payload-unary', 'BenchUnary', [
        'target' => $target,
        'duration_sec' => $durationSec,
        'payload_bytes' => $payloadBytes,
        'warmup_calls' => $warmupCalls,
        'max_calls' => $maxCalls,
        'diagnostic_rpc' => $diagnosticRpc,
        'client_internal_diagnostics' => $diagnosticRpc && $implementation === 'php-grpc-lite',
        'server_cached_payload' => $serverCachedPayload,
        'return_transfer_fast_path' => $returnTransferFastPath,
        'curl_trace_output' => $curlTraceOutput,
        'curl_trace_calls' => $curlTraceCalls,
        'transport' => $transport,
    ], $metrics);
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);
if (is_resource($curlTraceHandle)) {
    fclose($curlTraceHandle);
}

printf("%-22s %10s %12s %12s %12s\n", 'scenario', 'calls', 'calls/s', 'p50', 'p99');
printf("%'-72s\n", '');
foreach ($measurements as $measurement) {
    printf("%-22s %10d %12.1f %11.1fμs %11.1fμs\n", $measurement['name'], $measurement['metrics']['calls_total']['value'], $measurement['metrics']['calls_per_second']['value'], $measurement['metrics']['latency_p50_ns']['value'] / 1_000, $measurement['metrics']['latency_p99_ns']['value'] / 1_000);
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
function curlTraceCallback($handle, int $payloadBytes, int $callNumber): callable
{
    $startedNs = hrtime(true);
    fwrite($handle, sprintf("CALL\tpayload_bytes=%d\tcall=%d\n", $payloadBytes, $callNumber));

    return static function (int $type, string $data, int $nowNs) use ($handle, $payloadBytes, $callNumber, $startedNs): void {
        fwrite($handle, sprintf(
            "TRACE\tpayload_bytes=%d\tcall=%d\telapsed_us=%.1f\ttype=%s\tbytes=%d\t%s\n",
            $payloadBytes,
            $callNumber,
            ($nowNs - $startedNs) / 1_000,
            curlTraceTypeName($type),
            strlen($data),
            curlTraceDataSummary($type, $data),
        ));
    };
}

function curlTraceTypeName(int $type): string
{
    return match ($type) {
        \CURLINFO_TEXT => 'text',
        \CURLINFO_HEADER_IN => 'header_in',
        \CURLINFO_HEADER_OUT => 'header_out',
        \CURLINFO_DATA_IN => 'data_in',
        \CURLINFO_DATA_OUT => 'data_out',
        \CURLINFO_SSL_DATA_IN => 'ssl_data_in',
        \CURLINFO_SSL_DATA_OUT => 'ssl_data_out',
        default => "type_$type",
    };
}

function curlTraceDataSummary(int $type, string $data): string
{
    if ($type === \CURLINFO_DATA_IN || $type === \CURLINFO_DATA_OUT || $type === \CURLINFO_SSL_DATA_IN || $type === \CURLINFO_SSL_DATA_OUT) {
        return 'hex_prefix=' . bin2hex(substr($data, 0, 32));
    }

    $text = preg_replace('/[^\P{C}\t]/u', '.', $data);
    if (!is_string($text)) {
        $text = '';
    }
    $text = str_replace(["\r", "\n"], ['\\r', '\\n'], $text);
    return 'text=' . substr($text, 0, 500);
}

/**
 * @return array<string, list<string>>
 */
function diagnosticMetadata(bool $serverCachedPayload): array
{
    $metadata = [
        'x-bench-server-timing' => ['1'],
        'x-bench-server-stats' => ['1'],
    ];
    if ($serverCachedPayload) {
        $metadata['x-bench-server-cached-payload'] = ['1'];
    }
    return $metadata;
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
    fwrite(STDERR, "Usage: php tools/phase2/payload-unary.php --suite=payload-unary --implementation=php-grpc-lite --output=var/bench-results/result.json [--duration=1] [--payload-sizes=0,100,1024,10240,102400] [--warmup-calls=3] [--max-calls=0] [--diagnostic-rpc] [--server-cached-payload] [--return-transfer-fast-path] [--curl-trace-output=var/bench-results/trace.log --curl-trace-calls=3]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
