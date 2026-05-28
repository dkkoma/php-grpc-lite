<?php
declare(strict_types=1);

require_once __DIR__ . '/zts-parallel-worker.php';

$args = $argv;
array_shift($args);

$mode = 'thread';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$workers = [1, 2, 8];
$calls = 20;
$warmupCalls = 2;
$serverDelayMs = 10;
$streamMessages = 2;
$payloadBytes = 100;
$grpcExtension = '/workspace/ext/grpc/modules/grpc.so';
$workerChild = false;
$workerId = 0;
$case = 'unary';

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--mode') {
        $mode = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--mode=')) {
        $mode = substr($arg, strlen('--mode='));
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--workers') {
        $workers = parseWorkerList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--workers=')) {
        $workers = parseWorkerList(substr($arg, strlen('--workers=')));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif ($arg === '--server-delay-ms') {
        $serverDelayMs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--server-delay-ms=')) {
        $serverDelayMs = (int) substr($arg, strlen('--server-delay-ms='));
    } elseif ($arg === '--stream-messages') {
        $streamMessages = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--stream-messages=')) {
        $streamMessages = (int) substr($arg, strlen('--stream-messages='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--grpc-extension') {
        $grpcExtension = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--grpc-extension=')) {
        $grpcExtension = substr($arg, strlen('--grpc-extension='));
    } elseif ($arg === '--worker-child') {
        $workerChild = true;
    } elseif ($arg === '--worker-id') {
        $workerId = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--worker-id=')) {
        $workerId = (int) substr($arg, strlen('--worker-id='));
    } elseif ($arg === '--case') {
        $case = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--case=')) {
        $case = substr($arg, strlen('--case='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if (!in_array($mode, ['thread', 'process'], true)) {
    usage('mode must be thread or process');
}
if ($target === '' || $autoload === '' || $workers === [] || $calls <= 0 || $warmupCalls < 0 || $serverDelayMs < 0 || $streamMessages <= 0 || $payloadBytes < 0) {
    usage('target, autoload, workers, calls, warmup-calls, server-delay-ms, stream-messages, and payload-bytes must be valid');
}
if ($workerChild && !in_array($case, ['unary', 'streaming'], true)) {
    usage('worker child case must be unary or streaming');
}

if ($workerChild) {
    echo json_encode(zts_parallel_run_worker_case($autoload, $case, $target, $calls, $warmupCalls, $serverDelayMs, $streamMessages, $payloadBytes, $workerId), JSON_THROW_ON_ERROR), "\n";
    exit(0);
}

if ($mode === 'thread' && !extension_loaded('parallel')) {
    fwrite(STDERR, "parallel extension is required for --mode=thread\n");
    exit(1);
}

echo "mode,case,workers,total_calls,total_messages,total_wall_ms,throughput_per_sec,p50_ms,p95_ms,p99_ms,max_ms\n";
foreach ($workers as $workerCount) {
    foreach (['unary', 'streaming'] as $caseName) {
        $result = match ($mode) {
            'thread' => runThreadSet($caseName, $workerCount, $target, $autoload, $calls, $warmupCalls, $serverDelayMs, $streamMessages, $payloadBytes),
            'process' => runProcessSet($caseName, $workerCount, $target, $autoload, $calls, $warmupCalls, $serverDelayMs, $streamMessages, $payloadBytes, $grpcExtension),
        };
        printf(
            "%s,%s,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            $mode,
            $caseName,
            $workerCount,
            $result['total_calls'],
            $result['total_messages'],
            $result['total_wall_ms'],
            $result['throughput_per_sec'],
            $result['latency_ms']['p50'],
            $result['latency_ms']['p95'],
            $result['latency_ms']['p99'],
            $result['latency_ms']['max'],
        );
    }
}

/**
 * @return list<int>
 */
function parseWorkerList(string $value): array
{
    $workers = [];
    foreach (explode(',', $value) as $part) {
        $part = trim($part);
        if ($part === '') {
            continue;
        }
        $worker = (int) $part;
        if ($worker <= 0) {
            return [];
        }
        $workers[] = $worker;
    }

    return array_values(array_unique($workers));
}

/**
 * @return array{total_calls:int, total_messages:int, total_wall_ms:float, throughput_per_sec:float, latency_ms:array{p50:float,p95:float,p99:float,max:float}}
 */
function runThreadSet(string $case, int $workerCount, string $target, string $autoload, int $calls, int $warmupCalls, int $serverDelayMs, int $streamMessages, int $payloadBytes): array
{
    $futures = [];
    $workerPath = __DIR__ . '/zts-parallel-worker.php';
    $started = hrtime(true);
    for ($workerId = 0; $workerId < $workerCount; $workerId++) {
        $runtime = new parallel\Runtime();
        $futures[] = $runtime->run(static function (
            string $workerPath,
            string $autoload,
            string $case,
            string $target,
            int $calls,
            int $warmupCalls,
            int $serverDelayMs,
            int $streamMessages,
            int $payloadBytes,
            int $workerId,
        ): array {
            require_once $workerPath;
            return zts_parallel_run_worker_case($autoload, $case, $target, $calls, $warmupCalls, $serverDelayMs, $streamMessages, $payloadBytes, $workerId);
        }, [$workerPath, $autoload, $case, $target, $calls, $warmupCalls, $serverDelayMs, $streamMessages, $payloadBytes, $workerId]);
    }

    $workers = [];
    foreach ($futures as $future) {
        $workers[] = $future->value();
    }
    $ended = hrtime(true);

    return summarizeWorkers($workers, $ended - $started);
}

/**
 * @return array{total_calls:int, total_messages:int, total_wall_ms:float, throughput_per_sec:float, latency_ms:array{p50:float,p95:float,p99:float,max:float}}
 */
function runProcessSet(string $case, int $workerCount, string $target, string $autoload, int $calls, int $warmupCalls, int $serverDelayMs, int $streamMessages, int $payloadBytes, string $grpcExtension): array
{
    $processes = [];
    $script = $_SERVER['SCRIPT_FILENAME'];
    $started = hrtime(true);
    for ($workerId = 0; $workerId < $workerCount; $workerId++) {
        $command = [
            PHP_BINARY,
            '-d',
            "extension=$grpcExtension",
            $script,
            '--worker-child',
            "--case=$case",
            "--worker-id=$workerId",
            "--target=$target",
            "--autoload=$autoload",
            "--calls=$calls",
            "--warmup-calls=$warmupCalls",
            "--server-delay-ms=$serverDelayMs",
            "--stream-messages=$streamMessages",
            "--payload-bytes=$payloadBytes",
        ];
        $process = proc_open($command, [
            1 => ['pipe', 'w'],
            2 => ['pipe', 'w'],
        ], $pipes);
        if (!is_resource($process)) {
            throw new RuntimeException('failed to start process worker');
        }
        $processes[] = [$process, $pipes];
    }

    $workers = [];
    foreach ($processes as [$process, $pipes]) {
        $stdout = stream_get_contents($pipes[1]);
        $stderr = stream_get_contents($pipes[2]);
        fclose($pipes[1]);
        fclose($pipes[2]);
        $status = proc_close($process);
        if ($status !== 0) {
            throw new RuntimeException("process worker failed with status $status: $stderr");
        }
        $decoded = json_decode(trim($stdout), true, flags: JSON_THROW_ON_ERROR);
        if (!is_array($decoded)) {
            throw new RuntimeException('process worker returned invalid JSON');
        }
        $workers[] = $decoded;
    }
    $ended = hrtime(true);

    return summarizeWorkers($workers, $ended - $started);
}

/**
 * @param list<array{calls:int,messages:int,latencies_ns:list<int>}> $workers
 * @return array{total_calls:int, total_messages:int, total_wall_ms:float, throughput_per_sec:float, latency_ms:array{p50:float,p95:float,p99:float,max:float}}
 */
function summarizeWorkers(array $workers, int $wallNs): array
{
    $calls = 0;
    $messages = 0;
    $latencies = [];
    foreach ($workers as $worker) {
        $calls += (int) $worker['calls'];
        $messages += (int) $worker['messages'];
        foreach ($worker['latencies_ns'] as $latencyNs) {
            $latencies[] = ((float) $latencyNs) / 1_000_000.0;
        }
    }
    sort($latencies, SORT_NUMERIC);
    $wallSec = ((float) $wallNs) / 1_000_000_000.0;

    return [
        'total_calls' => $calls,
        'total_messages' => $messages,
        'total_wall_ms' => $wallSec * 1000.0,
        'throughput_per_sec' => $calls / max($wallSec, 0.000001),
        'latency_ms' => [
            'p50' => percentile($latencies, 50.0),
            'p95' => percentile($latencies, 95.0),
            'p99' => percentile($latencies, 99.0),
            'max' => $latencies[count($latencies) - 1] ?? 0.0,
        ],
    ];
}

/**
 * @param list<float> $sortedValues
 */
function percentile(array $sortedValues, float $percentile): float
{
    if ($sortedValues === []) {
        return 0.0;
    }
    $index = (int) ceil(($percentile / 100.0) * count($sortedValues)) - 1;
    $index = max(0, min(count($sortedValues) - 1, $index));

    return $sortedValues[$index];
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/zts-parallel-call-path.php --mode=thread|process [--workers=1,2,8] [--calls=20] [--server-delay-ms=10] [--stream-messages=2]\n");
    exit(2);
}
