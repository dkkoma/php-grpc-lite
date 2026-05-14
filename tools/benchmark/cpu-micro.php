<?php
declare(strict_types=1);

require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\StreamingBenchHelper;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'cpu-micro';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$calls = 5000;
$warmupCalls = 100;
$nativeResponseMode = 'stream';
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
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif ($arg === '--native-response-mode') {
        $nativeResponseMode = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--native-response-mode=')) {
        $nativeResponseMode = substr($arg, strlen('--native-response-mode='));
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
if ($calls <= 0 || $warmupCalls < 0) {
    usage('calls and warmup-calls must be valid');
}
if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$clientOptions = ['php_grpc_lite.native_response_mode' => $nativeResponseMode];
$unaryClient = UnaryBenchHelper::client($target, $clientOptions);
$streamingClient = StreamingBenchHelper::client($target, $clientOptions);

$cases = [
    [
        'name' => 'small_unary_100b',
        'call_type' => 'unary',
        'request' => unaryRequest(100, 100),
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 1,
    ],
    [
        'name' => 'begin_txn_unary',
        'call_type' => 'unary',
        'request' => unaryRequest(92, 18),
        'request_bytes' => 92,
        'response_bytes' => 18,
        'message_count' => 1,
    ],
    [
        'name' => 'commit_txn_unary',
        'call_type' => 'unary',
        'request' => unaryRequest(106, 14),
        'request_bytes' => 106,
        'response_bytes' => 14,
        'message_count' => 1,
    ],
    [
        'name' => 'small_streaming_1x100b',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(1, 100, 100),
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 1,
    ],
    [
        'name' => 'small_streaming_100x100b',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(100, 100, 100),
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 100,
    ],
    [
        'name' => 'select_1row_10col_streaming',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(1, 160, 100),
        'request_bytes' => 160,
        'response_bytes' => 100,
        'message_count' => 1,
    ],
    [
        'name' => 'dml_insert_10col_streaming',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(1, 355, 8),
        'request_bytes' => 355,
        'response_bytes' => 8,
        'message_count' => 1,
    ],
    [
        'name' => 'dml_update_10col_streaming',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(1, 327, 8),
        'request_bytes' => 327,
        'response_bytes' => 8,
        'message_count' => 1,
    ],
    [
        'name' => 'dml_delete_10col_streaming',
        'call_type' => 'server_streaming',
        'request' => streamingRequest(1, 144, 8),
        'request_bytes' => 144,
        'response_bytes' => 8,
        'message_count' => 1,
    ],
];

printf(
    "%-32s %-16s %8s %14s %14s %14s %14s\n",
    'measurement',
    'type',
    'calls',
    'cpu_us/call',
    'user_us/call',
    'sys_us/call',
    'wall_us/call',
);
printf("%'-116s\n", '');

foreach ($cases as $case) {
    $benchTelemetry->setContext($case['name'], commonContext($target, $calls, $warmupCalls, $transport) + [
        'benchmark.call_type' => $case['call_type'],
        'benchmark.request_bytes' => $case['request_bytes'],
        'benchmark.response_bytes' => $case['response_bytes'],
        'benchmark.message_count' => $case['message_count'],
        'benchmark.native_response_mode' => $nativeResponseMode,
        'benchmark.operation_shape' => $case['name'],
    ]);

    runWarmup($case, $unaryClient, $streamingClient, $warmupCalls);
    $result = measureCase($case, $unaryClient, $streamingClient, $calls);

    printf(
        "%-32s %-16s %8d %14.1f %14.1f %14.1f %14.1f\n",
        $case['name'],
        $case['call_type'],
        $calls,
        $result['cpu_total_us_per_call'],
        $result['cpu_user_us_per_call'],
        $result['cpu_sys_us_per_call'],
        $result['wall_us_per_call'],
    );

    $benchTelemetry->recordMetricSpan('CpuMicroSummary', $result['start_ns'], $result['end_ns'], [
        'benchmark.metric_kind' => 'cpu_summary',
        'benchmark.cpu_total_us' => $result['cpu_total_us'],
        'benchmark.cpu_user_us' => $result['cpu_user_us'],
        'benchmark.cpu_sys_us' => $result['cpu_sys_us'],
        'benchmark.cpu_total_us_per_call' => $result['cpu_total_us_per_call'],
        'benchmark.cpu_user_us_per_call' => $result['cpu_user_us_per_call'],
        'benchmark.cpu_sys_us_per_call' => $result['cpu_sys_us_per_call'],
        'benchmark.wall_total_us' => $result['wall_total_us'],
        'benchmark.wall_us_per_call' => $result['wall_us_per_call'],
    ]);
}

echo "OTEL CPU summary spans exported.\n";

function unaryRequest(int $requestBytes, int $responseBytes): BenchRequest
{
    return UnaryBenchHelper::request($responseBytes, 0, str_repeat("\0", $requestBytes));
}

function streamingRequest(int $messageCount, int $requestBytes, int $responseBytes): BenchRequest
{
    $request = StreamingBenchHelper::request($messageCount, $responseBytes);
    $request->setRequestPayload(str_repeat("\0", $requestBytes));
    return $request;
}

/** @return array<string, int|string> */
function commonContext(string $target, int $calls, int $warmupCalls, string $transport): array
{
    return [
        'benchmark.target' => $target,
        'benchmark.calls' => $calls,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.transport' => $transport,
        'benchmark.cpu_source' => 'getrusage',
    ];
}

/** @param array<string, mixed> $case */
function runWarmup(array $case, object $unaryClient, object $streamingClient, int $warmupCalls): void
{
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        runOneCall($case, $unaryClient, $streamingClient);
    }
}

/**
 * @param array<string, mixed> $case
 * @return array<string, float|int>
 */
function measureCase(array $case, object $unaryClient, object $streamingClient, int $calls): array
{
    $usageStart = getrusage();
    $startNs = hrtime(true);
    for ($call = 0; $call < $calls; $call++) {
        runOneCall($case, $unaryClient, $streamingClient);
    }
    $endNs = hrtime(true);
    $usageEnd = getrusage();

    $cpuUserUs = rusageTimeUs($usageEnd, 'ru_utime') - rusageTimeUs($usageStart, 'ru_utime');
    $cpuSysUs = rusageTimeUs($usageEnd, 'ru_stime') - rusageTimeUs($usageStart, 'ru_stime');
    $cpuTotalUs = $cpuUserUs + $cpuSysUs;
    $wallTotalUs = ($endNs - $startNs) / 1000.0;

    return [
        'start_ns' => $startNs,
        'end_ns' => $endNs,
        'cpu_user_us' => $cpuUserUs,
        'cpu_sys_us' => $cpuSysUs,
        'cpu_total_us' => $cpuTotalUs,
        'cpu_user_us_per_call' => $cpuUserUs / $calls,
        'cpu_sys_us_per_call' => $cpuSysUs / $calls,
        'cpu_total_us_per_call' => $cpuTotalUs / $calls,
        'wall_total_us' => $wallTotalUs,
        'wall_us_per_call' => $wallTotalUs / $calls,
    ];
}

/** @param array<string, mixed> $case */
function runOneCall(array $case, object $unaryClient, object $streamingClient): void
{
    if ($case['call_type'] === 'unary') {
        UnaryBenchHelper::call($unaryClient, $case['request']);
        return;
    }

    StreamingBenchHelper::drain($streamingClient, $case['request']);
}

/** @param array<string, mixed> $usage */
function rusageTimeUs(array $usage, string $prefix): float
{
    return ((float) ($usage[$prefix . '.tv_sec'] ?? 0)) * 1_000_000.0
        + (float) ($usage[$prefix . '.tv_usec'] ?? 0);
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/cpu-micro.php --suite=cpu-micro --implementation=php-grpc-lite [--calls=5000] [--warmup-calls=100]\n");
    exit(2);
}
