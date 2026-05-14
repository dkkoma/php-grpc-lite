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

$suite = 'cpu-concurrent';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$transport = 'native';
$workers = 4;
$calls = 5000;
$warmupCalls = 100;
$workerCase = '';

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
    } elseif ($arg === '--transport') {
        $transport = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--transport=')) {
        $transport = substr($arg, strlen('--transport='));
    } elseif ($arg === '--workers') {
        $workers = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--workers=')) {
        $workers = (int) substr($arg, strlen('--workers='));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif ($arg === '--worker-case') {
        $workerCase = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--worker-case=')) {
        $workerCase = substr($arg, strlen('--worker-case='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '') {
    usage('suite, implementation, target, and autoload are required');
}
if ($workers <= 0 || $calls <= 0 || $warmupCalls < 0) {
    usage('workers, calls, and warmup-calls must be valid');
}
if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$cases = [
    'small_unary_100b' => [
        'call_type' => 'unary',
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 1,
    ],
    'small_streaming_1x100b' => [
        'call_type' => 'server_streaming',
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 1,
    ],
    'small_streaming_100x100b' => [
        'call_type' => 'server_streaming',
        'request_bytes' => 100,
        'response_bytes' => 100,
        'message_count' => 100,
    ],
];

if ($workerCase !== '') {
    runWorker($cases[$workerCase] ?? null, $target, $calls, $warmupCalls);
    exit(0);
}

$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

printf(
    "%-28s %8s %10s %14s %14s %14s %14s\n",
    'measurement',
    'workers',
    'calls',
    'cpu_us/call',
    'user_us/call',
    'sys_us/call',
    'wall_us/call',
);
printf("%'-112s\n", '');

foreach ($cases as $caseName => $case) {
    $totalCalls = $workers * $calls;
    $benchTelemetry->setContext($caseName, [
        'benchmark.target' => $target,
        'benchmark.calls' => $totalCalls,
        'benchmark.calls_per_worker' => $calls,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.workers' => $workers,
        'benchmark.transport' => $transport,
        'benchmark.cpu_source' => 'getrusage_children',
        'benchmark.call_type' => $case['call_type'],
        'benchmark.request_bytes' => $case['request_bytes'],
        'benchmark.response_bytes' => $case['response_bytes'],
        'benchmark.message_count' => $case['message_count'],
        'benchmark.operation_shape' => $caseName,
    ]);

    $result = measureConcurrentCase($caseName, $implementation, $target, $autoload, $workers, $calls, $warmupCalls);
    printf(
        "%-28s %8d %10d %14.1f %14.1f %14.1f %14.1f\n",
        $caseName,
        $workers,
        $totalCalls,
        $result['cpu_total_us_per_call'],
        $result['cpu_user_us_per_call'],
        $result['cpu_sys_us_per_call'],
        $result['wall_us_per_call'],
    );

    $benchTelemetry->recordMetricSpan('CpuConcurrentSummary', $result['start_ns'], $result['end_ns'], [
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

echo "OTEL concurrent CPU summary spans exported.\n";

/** @param array<string, mixed>|null $case */
function runWorker(?array $case, string $target, int $calls, int $warmupCalls): void
{
    if ($case === null) {
        throw new RuntimeException('unknown worker case');
    }

    if ($case['call_type'] === 'unary') {
        $client = UnaryBenchHelper::client($target);
        $request = unaryRequest($case['request_bytes'], $case['response_bytes']);
        for ($index = 0; $index < $warmupCalls; $index++) {
            UnaryBenchHelper::call($client, $request);
        }
        waitForStartSignal();
        $usageStart = getrusage();
        $startNs = hrtime(true);
        for ($index = 0; $index < $calls; $index++) {
            UnaryBenchHelper::call($client, $request);
        }
        writeWorkerResult($usageStart, $startNs);
        return;
    }

    $client = StreamingBenchHelper::client($target);
    $request = streamingRequest($case['message_count'], $case['request_bytes'], $case['response_bytes']);
    for ($index = 0; $index < $warmupCalls; $index++) {
        StreamingBenchHelper::drain($client, $request);
    }
    waitForStartSignal();
    $usageStart = getrusage();
    $startNs = hrtime(true);
    for ($index = 0; $index < $calls; $index++) {
        StreamingBenchHelper::drain($client, $request);
    }
    writeWorkerResult($usageStart, $startNs);
}

function waitForStartSignal(): void
{
    fwrite(STDOUT, "READY\n");
    fflush(STDOUT);
    $line = fgets(STDIN);
    if ($line === false || trim($line) !== 'GO') {
        throw new RuntimeException('worker did not receive start signal');
    }
}

/** @param array<string, mixed> $usageStart */
function writeWorkerResult(array $usageStart, int $startNs): void
{
    $endNs = hrtime(true);
    $usageEnd = getrusage();
    $payload = [
        'cpu_user_us' => rusageTimeUs($usageEnd, 'ru_utime') - rusageTimeUs($usageStart, 'ru_utime'),
        'cpu_sys_us' => rusageTimeUs($usageEnd, 'ru_stime') - rusageTimeUs($usageStart, 'ru_stime'),
        'wall_us' => ($endNs - $startNs) / 1000.0,
    ];
    fwrite(STDOUT, json_encode($payload, JSON_THROW_ON_ERROR) . "\n");
    fflush(STDOUT);
}

/**
 * @return array<string, float|int>
 */
function measureConcurrentCase(
    string $caseName,
    string $implementation,
    string $target,
    string $autoload,
    int $workers,
    int $calls,
    int $warmupCalls,
): array {
    $processes = [];

    for ($worker = 0; $worker < $workers; $worker++) {
        $command = workerCommand($implementation, $target, $autoload, $caseName, $calls, $warmupCalls);
        $process = proc_open($command, [
            0 => ['pipe', 'r'],
            1 => ['pipe', 'w'],
            2 => ['pipe', 'w'],
        ], $pipes);
        if (!is_resource($process)) {
            throw new RuntimeException('failed to start worker process');
        }
        $processes[] = [$process, $pipes];
    }

    foreach ($processes as [$process, $pipes]) {
        $ready = fgets($pipes[1]);
        if ($ready === false || trim($ready) !== 'READY') {
            throw new RuntimeException('worker did not become ready');
        }
    }

    foreach ($processes as [$process, $pipes]) {
        fwrite($pipes[0], "GO\n");
        fclose($pipes[0]);
    }

    $startNs = hrtime(true);
    $cpuUserUs = 0.0;
    $cpuSysUs = 0.0;

    foreach ($processes as [$process, $pipes]) {
        $stdout = stream_get_contents($pipes[1]);
        $stderr = stream_get_contents($pipes[2]);
        fclose($pipes[1]);
        fclose($pipes[2]);
        $exitCode = proc_close($process);
        if ($exitCode !== 0) {
            throw new RuntimeException("worker failed with code $exitCode\nstdout:\n$stdout\nstderr:\n$stderr");
        }
        $result = parseWorkerResult($stdout);
        $cpuUserUs += $result['cpu_user_us'];
        $cpuSysUs += $result['cpu_sys_us'];
    }

    $endNs = hrtime(true);
    $cpuTotalUs = $cpuUserUs + $cpuSysUs;
    $wallTotalUs = ($endNs - $startNs) / 1000.0;
    $totalCalls = $workers * $calls;

    return [
        'start_ns' => $startNs,
        'end_ns' => $endNs,
        'cpu_user_us' => $cpuUserUs,
        'cpu_sys_us' => $cpuSysUs,
        'cpu_total_us' => $cpuTotalUs,
        'cpu_user_us_per_call' => $cpuUserUs / $totalCalls,
        'cpu_sys_us_per_call' => $cpuSysUs / $totalCalls,
        'cpu_total_us_per_call' => $cpuTotalUs / $totalCalls,
        'wall_total_us' => $wallTotalUs,
        'wall_us_per_call' => $wallTotalUs / $totalCalls,
    ];
}

/** @return array{cpu_user_us: float, cpu_sys_us: float, wall_us: float} */
function parseWorkerResult(string $stdout): array
{
    $lines = array_values(array_filter(array_map('trim', explode("\n", $stdout))));
    $json = end($lines);
    if (!is_string($json) || $json === '') {
        throw new RuntimeException("worker did not return result\nstdout:\n$stdout");
    }

    $decoded = json_decode($json, true, flags: JSON_THROW_ON_ERROR);
    if (!is_array($decoded)) {
        throw new RuntimeException("worker returned invalid result\nstdout:\n$stdout");
    }

    return [
        'cpu_user_us' => (float) ($decoded['cpu_user_us'] ?? 0.0),
        'cpu_sys_us' => (float) ($decoded['cpu_sys_us'] ?? 0.0),
        'wall_us' => (float) ($decoded['wall_us'] ?? 0.0),
    ];
}

/**
 * @return list<string>
 */
function workerCommand(string $implementation, string $target, string $autoload, string $caseName, int $calls, int $warmupCalls): array
{
    $command = [PHP_BINARY];
    if ($implementation === 'php-grpc-lite') {
        $command[] = '-d';
        $command[] = 'extension=/workspace/ext/grpc/modules/grpc.so';
    }
    $command[] = __FILE__;
    $command[] = '--worker-case=' . $caseName;
    $command[] = '--target=' . $target;
    $command[] = '--autoload=' . $autoload;
    $command[] = '--calls=' . (string) $calls;
    $command[] = '--warmup-calls=' . (string) $warmupCalls;

    return $command;
}

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

/** @param array<string, mixed> $usage */
function rusageTimeUs(array $usage, string $prefix): float
{
    return ((float) ($usage[$prefix . '.tv_sec'] ?? 0)) * 1_000_000.0
        + (float) ($usage[$prefix . '.tv_usec'] ?? 0);
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/cpu-concurrent.php --suite=cpu-concurrent --implementation=php-grpc-lite [--workers=4] [--calls=5000] [--warmup-calls=100]\n");
    exit(2);
}
