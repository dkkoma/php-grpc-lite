<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\ResourceSampler;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'spanner-dml-unary-shape';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 1.0;
$warmupCalls = 3;
$maxCalls = 0;
$transport = 'native';
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
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($durationSec <= 0 || $warmupCalls < 0 || $maxCalls < 0) {
    usage('duration, warmup-calls, and max-calls must be valid');
}
if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}
require $autoload;
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$clientOptions = [];
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$client = UnaryBenchHelper::client($target, $clientOptions);

foreach ($cases as $case) {
    $requestPayload = $case['request_bytes'] > 0 ? str_repeat("\0", $case['request_bytes']) : '';
    $request = UnaryBenchHelper::request($case['response_bytes'], 0, $requestPayload);
    $benchTelemetry->setContext($case['name'], [
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
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $maxCalls, $benchTelemetry, &$latenciesNs): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            $statusCode = 1;
            try {
                UnaryBenchHelper::call($client, $request);
            } catch (\Throwable $throwable) {
                $statusCode = 2;
                throw $throwable;
            } finally {
                $callEndNs = hrtime(true);
                $benchTelemetry->recordRpcSpan('BenchUnary', $callStartNs, $callEndNs, [
                    'rpc.service' => 'helloworld.Greeter',
                    'rpc.method' => 'BenchUnary',
                ], $statusCode);
            }
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

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/unary-shape.php --suite=spanner-dml-unary-shape --implementation=php-grpc-lite [--duration=1] [--warmup-calls=3] [--max-calls=0]\n");
    exit(2);
}
