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

$suite = 'unary-shape';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 1.0;
$warmupCalls = 3;
$maxCalls = 0;
$transport = 'native';
$diagnosticRpc = false;
$cases = [
    ['name' => 'begin_txn', 'request_bytes' => 92, 'response_bytes' => 18],
    ['name' => 'dml_insert_10col', 'request_bytes' => 355, 'response_bytes' => 8],
    ['name' => 'dml_update_10col', 'request_bytes' => 327, 'response_bytes' => 8],
    ['name' => 'dml_delete_10col', 'request_bytes' => 144, 'response_bytes' => 8],
    ['name' => 'commit_txn', 'request_bytes' => 106, 'response_bytes' => 14],
];

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
    } elseif ($arg === '--diagnostic-rpc') {
        $diagnosticRpc = true;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}
if ($durationSec <= 0 || $warmupCalls < 0 || $maxCalls < 0) {
    usage('duration, warmup-calls, and max-calls must be valid');
}
if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}
require $autoload;
$benchTelemetry = BenchTelemetry::fromEnvironment($suite, $implementation);
if ($benchTelemetry !== null) {
    register_shutdown_function([$benchTelemetry, 'shutdown']);
}

$clientOptions = [];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = UnaryBenchHelper::client($target, $clientOptions);
$measurements = [];

foreach ($cases as $case) {
    $requestPayload = $case['request_bytes'] > 0 ? str_repeat("\0", $case['request_bytes']) : '';
    $request = UnaryBenchHelper::request($case['response_bytes'], 0, $requestPayload);
    $benchTelemetry?->setContext($case['name'], [
        'benchmark.target' => $target,
        'benchmark.duration_sec' => $durationSec,
        'benchmark.request_bytes' => $case['request_bytes'],
        'benchmark.response_bytes' => $case['response_bytes'],
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.max_calls' => $maxCalls,
        'benchmark.transport' => $transport,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($client, $request);
    }

    $latenciesNs = [];
    $diagnosticSeries = [];
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $maxCalls, $diagnosticRpc, $implementation, $benchTelemetry, &$latenciesNs, &$diagnosticSeries): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            $callRunner = static function () use ($client, $request, $diagnosticRpc, $implementation, &$diagnosticSeries): void {
                if ($diagnosticRpc) {
                    $diagnostics = new \stdClass();
                    $options = [];
                    if ($implementation === 'php-grpc-lite') {
                        $options['php_grpc_lite.diagnostics'] = $diagnostics;
                    }
                    UnaryBenchHelper::call($client, $request, $options);
                    if ($implementation === 'php-grpc-lite') {
                        collectDiagnostics($diagnostics, $diagnosticSeries);
                    }
                    return;
                }
                UnaryBenchHelper::call($client, $request);
            };
            if ($benchTelemetry !== null) {
                $benchTelemetry->measureRpc('BenchUnary', [
                    'rpc.service' => 'helloworld.Greeter',
                    'rpc.method' => 'BenchUnary',
                    'benchmark.phase' => 'measurement',
                ], $callRunner);
            } else {
                $callRunner();
            }
            $latenciesNs[] = hrtime(true) - $callStartNs;
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

    $measurements[] = ResultContract::measurement($case['name'], 'unary-shape', 'BenchUnary', [
        'target' => $target,
        'duration_sec' => $durationSec,
        'request_bytes' => $case['request_bytes'],
        'response_bytes' => $case['response_bytes'],
        'warmup_calls' => $warmupCalls,
        'max_calls' => $maxCalls,
        'transport' => $transport,
        'diagnostic_rpc' => $diagnosticRpc,
    ], $metrics);
}

$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, ResultContract::encode(ResultContract::document($suite, $implementation, $measurements)));

printf("%-18s %10s %10s %10s %12s %12s %12s\n", 'case', 'req_b', 'resp_b', 'calls', 'calls/s', 'p50', 'p99');
printf("%'-90s\n", '');
foreach ($measurements as $measurement) {
    printf(
        "%-18s %10d %10d %10d %12.1f %11.1fμs %11.1fμs\n",
        $measurement['name'],
        $measurement['attributes']->request_bytes,
        $measurement['attributes']->response_bytes,
        $measurement['metrics']['calls_total']['value'],
        $measurement['metrics']['calls_per_second']['value'],
        $measurement['metrics']['latency_p50_ns']['value'] / 1_000,
        $measurement['metrics']['latency_p99_ns']['value'] / 1_000,
    );
}
echo "JSON: $output\n";

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/unary-shape.php --suite=unary-shape --implementation=php-grpc-lite --output=var/bench-results/result.json [--duration=1] [--warmup-calls=3] [--max-calls=0]\n");
    exit(2);
}

/** @param array<string, list<int|float>> $series */
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
 * @param array<string, list<int|float>> $series
 * @return array<string, array{value: int|float, unit: string}>
 */
function summarizeDiagnostics(array $series): array
{
    $metrics = [];
    foreach ($series as $name => $values) {
        foreach (UnaryBenchHelper::percentiles($values) as $percentile => $value) {
            $metrics[$name . '_' . $percentile] = [
                'value' => $value,
                'unit' => str_ends_with($name, '_bytes') || str_ends_with($name, '_frames') ? 'count' : 'ns',
            ];
        }
    }
    return $metrics;
}
