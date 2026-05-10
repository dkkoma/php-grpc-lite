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

$suite = 'throughput-unary';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$durationSec = 3.0;
$payloadBytes = 100;
$serverDelayMs = 0;
$warmupCalls = 10;
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
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--server-delay-ms') {
        $serverDelayMs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--server-delay-ms=')) {
        $serverDelayMs = (int) substr($arg, strlen('--server-delay-ms='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
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
if ($output === null || $output === '') {
    usage('output is required');
}
if ($durationSec <= 0 || $payloadBytes < 0 || $serverDelayMs < 0 || $warmupCalls < 0) {
    usage('duration, payload-bytes, server-delay-ms, and warmup-calls must be non-negative');
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
$client = UnaryBenchHelper::client($target, $clientOptions);
$request = UnaryBenchHelper::request($payloadBytes, $serverDelayMs);
$benchTelemetry?->setContext('throughput_unary', [
    'benchmark.target' => $target,
    'benchmark.duration_sec' => $durationSec,
    'benchmark.payload_bytes' => $payloadBytes,
    'benchmark.server_delay_ms' => $serverDelayMs,
    'benchmark.warmup_calls' => $warmupCalls,
    'benchmark.transport' => $transport,
]);
for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
    UnaryBenchHelper::call($client, $request);
}

$latenciesNs = [];
$deadlineNs = (int) round($durationSec * 1_000_000_000);
$sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, $benchTelemetry, &$latenciesNs): int {
    $startedNs = hrtime(true);
    $calls = 0;

    do {
        $callStartNs = hrtime(true);
        UnaryBenchHelper::call($client, $request);
        $callEndNs = hrtime(true);
        $benchTelemetry?->recordRpcSpan('BenchUnary', $callStartNs, $callEndNs, [
            'rpc.service' => 'helloworld.Greeter',
            'rpc.method' => 'BenchUnary',
            'benchmark.phase' => 'measurement',
        ]);
        $latenciesNs[] = $callEndNs - $callStartNs;
        $calls++;
    } while (hrtime(true) - $startedNs < $deadlineNs);

    return $calls;
});

$calls = $sample['result'];
$metrics = $sample['metrics'];
$elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
$percentiles = UnaryBenchHelper::percentiles($latenciesNs);
$metrics['calls_total'] = [
    'value' => $calls,
    'unit' => 'calls',
];
$metrics['calls_per_second'] = [
    'value' => $calls / $elapsedSec,
    'unit' => 'calls/s',
];
$metrics['wall_time_ns_per_call'] = [
    'value' => $metrics['wall_time_ns_total']['value'] / $calls,
    'unit' => 'ns/call',
];
$metrics['diagnostic_cpu_total_us_per_call'] = [
    'value' => $metrics['diagnostic_cpu_total_us_total']['value'] / $calls,
    'unit' => 'us/call',
];
foreach ($percentiles as $name => $value) {
    $metrics['latency_' . $name . '_ns'] = [
        'value' => $value,
        'unit' => 'ns',
    ];
}

$document = ResultContract::document(
    $suite,
    $implementation,
    [
        ResultContract::measurement(
            'throughput_unary',
            'throughput',
            'BenchUnary',
            [
                'target' => $target,
                'duration_sec' => $durationSec,
                'payload_bytes' => $payloadBytes,
                'server_delay_ms' => $serverDelayMs,
                'warmup_calls' => $warmupCalls,
                'concurrency' => 1,
                'transport' => $transport,
            ],
            $metrics,
        ),
    ],
);

$encoded = ResultContract::encode($document);
$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, $encoded);

printf("%-22s %10s %12s %12s %12s %12s\n", 'scenario', 'calls', 'calls/s', 'p50', 'p95', 'p99');
printf("%'-86s\n", '');
printf(
    "%-22s %10d %12.1f %11.1fμs %11.1fμs %11.1fμs\n",
    'throughput_unary',
    $calls,
    $metrics['calls_per_second']['value'],
    $metrics['latency_p50_ns']['value'] / 1_000,
    $metrics['latency_p95_ns']['value'] / 1_000,
    $metrics['latency_p99_ns']['value'] / 1_000,
);
echo "JSON: $output\n";

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(
        STDERR,
        "Usage: php tools/phase2/throughput-unary.php --suite=throughput-unary --implementation=php-grpc-lite --output=var/bench-results/result.json [--target=test-server:50051] [--duration=3] [--payload-bytes=100] [--server-delay-ms=0]\n",
    );
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
