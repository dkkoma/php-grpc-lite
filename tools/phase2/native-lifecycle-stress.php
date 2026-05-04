<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/StreamingBenchHelper.php';

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\StreamingBenchHelper;

$args = $argv;
array_shift($args);

$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$iterations = 100;
$messageCount = 20;
$payloadBytes = 1024;
$sleepUs = 0;

for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--output') {
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
    } elseif ($arg === '--iterations') {
        $iterations = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--iterations=')) {
        $iterations = (int) substr($arg, strlen('--iterations='));
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--sleep-us') {
        $sleepUs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--sleep-us=')) {
        $sleepUs = (int) substr($arg, strlen('--sleep-us='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($output === null || $output === '' || $iterations <= 0 || $messageCount <= 0 || $payloadBytes < 0 || $sleepUs < 0) {
    usage('output, iterations, message-count, payload-bytes, and sleep-us must be valid');
}
if (!extension_loaded('nghttp2_poc')) {
    throw new \RuntimeException('nghttp2_poc extension is required');
}
if (!is_file($autoload)) {
    throw new \RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$client = new GreeterClient($target, [
    'credentials' => ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'native',
]);
$request = StreamingBenchHelper::request($messageCount, $payloadBytes);
$measurements = [];

$measurements[] = runScenario('full_drain_repeat', $iterations, static function () use ($client, $request): void {
    $call = $client->BenchServerStream($request);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    $status = $call->getStatus();
    if ($status->code !== \Grpc\STATUS_OK) {
        throw new \RuntimeException("unexpected status after full drain: {$status->code}: {$status->details}");
    }
    if ($count <= 0) {
        throw new \RuntimeException('full drain yielded no messages');
    }
});

$measurements[] = runScenario('break_unset_repeat', $iterations, static function () use ($client, $request): void {
    $call = $client->BenchServerStream($request);
    $responses = $call->responses();
    foreach ($responses as $_reply) {
        break;
    }
    unset($responses, $call);
    gc_collect_cycles();
    probeUnary($client);
});

$measurements[] = runScenario('cancel_mid_stream_repeat', $iterations, static function () use ($client, $request): void {
    $call = $client->BenchServerStream($request);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
        $call->cancel();
    }
    if ($count !== 1) {
        throw new \RuntimeException("cancel scenario expected 1 message, got $count");
    }
    $status = $call->getStatus();
    if ($status->code !== \Grpc\STATUS_CANCELLED) {
        throw new \RuntimeException("unexpected cancel status: {$status->code}: {$status->details}");
    }
    probeUnary($client);
});

$delayedRequest = StreamingBenchHelper::request(max(2, $messageCount), $payloadBytes, $sleepUs > 0 ? max(1, (int) ceil($sleepUs / 1000)) : 50);
$measurements[] = runScenario('timeout_repeat', $iterations, static function () use ($client, $delayedRequest): void {
    $call = $client->BenchServerStream($delayedRequest, [], [
        'timeout' => 10_000,
    ]);
    foreach ($call->responses() as $_reply) {
    }
    $status = $call->getStatus();
    if ($status->code !== \Grpc\STATUS_DEADLINE_EXCEEDED) {
        throw new \RuntimeException("unexpected timeout status: {$status->code}: {$status->details}");
    }
    probeUnary($client);
});

$rawResourceKey = 'lifecycle-stress-' . getmypid() . '-' . bin2hex(random_bytes(4));
$measurements[] = runScenario('raw_resource_unset_repeat', $iterations, static function () use ($request, $rawResourceKey): void {
    $serialized = $request->serializeToString();
    $stream = \nghttp2_poc_stream_open(
        $rawResourceKey,
        'test-server',
        50051,
        '/helloworld.Greeter/BenchServerStream',
        $serialized,
        [],
    );
    $next = \nghttp2_poc_stream_next($stream);
    if (($next['done'] ?? false) === true || !is_string($next['payload'] ?? null)) {
        throw new \RuntimeException('raw stream did not yield a payload');
    }
    unset($stream);
    gc_collect_cycles();

    $probe = \nghttp2_poc_stream_open(
        $rawResourceKey,
        'test-server',
        50051,
        '/helloworld.Greeter/BenchServerStream',
        $serialized,
        [],
    );
    $probeNext = \nghttp2_poc_stream_next($probe);
    if (($probeNext['done'] ?? false) === true || !is_string($probeNext['payload'] ?? null)) {
        throw new \RuntimeException('raw stream probe did not yield a payload');
    }
    \nghttp2_poc_stream_cancel($probe);
});

$document = ResultContract::document('native-lifecycle-stress', 'php-grpc-lite', $measurements);
$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, ResultContract::encode($document));

foreach ($measurements as $measurement) {
    $metrics = $measurement['metrics'];
    printf(
        "%-28s iterations=%d failures=%d wall=%.1fms php-delta=%dB rss-delta=%sKiB fd-delta=%s\n",
        $measurement['name'],
        $metrics['iterations_total']['value'],
        $metrics['failures_total']['value'],
        $metrics['wall_time_ns_total']['value'] / 1_000_000,
        $metrics['memory_usage_delta_bytes']['value'],
        $metrics['rss_max_delta_kib']['value'] ?? 'n/a',
        $metrics['fd_count_delta']['value'] ?? 'n/a',
    );
}
echo "JSON: $output\n";

/** @return array<string, mixed> */
function runScenario(string $name, int $iterations, callable $callback): array
{
    gc_collect_cycles();
    $memoryStart = memory_get_usage(false);
    $memoryRealStart = memory_get_usage(true);
    $rssStart = rssKiB();
    $fdStart = fdCount();
    $memoryMax = $memoryStart;
    $memoryRealMax = $memoryRealStart;
    $rssMax = $rssStart;
    $fdMax = $fdStart;
    $failures = 0;
    $startedNs = hrtime(true);

    for ($iteration = 0; $iteration < $iterations; $iteration++) {
        try {
            $callback($iteration);
        } catch (\Throwable $e) {
            $failures++;
            throw new \RuntimeException("$name failed at iteration $iteration: {$e->getMessage()}", 0, $e);
        } finally {
            gc_collect_cycles();
            $memoryMax = max($memoryMax, memory_get_usage(false));
            $memoryRealMax = max($memoryRealMax, memory_get_usage(true));
            $rss = rssKiB();
            if ($rss !== null) {
                $rssMax = $rssMax === null ? $rss : max($rssMax, $rss);
            }
            $fds = fdCount();
            if ($fds !== null) {
                $fdMax = $fdMax === null ? $fds : max($fdMax, $fds);
            }
        }
    }

    $elapsedNs = hrtime(true) - $startedNs;
    $memoryEnd = memory_get_usage(false);
    $memoryRealEnd = memory_get_usage(true);
    $rssEnd = rssKiB();
    $fdEnd = fdCount();

    return ResultContract::measurement($name, 'native-lifecycle', 'BenchServerStream', [
        'target' => 'test-server:50051',
        'transport' => 'native',
    ], [
        'iterations_total' => ['value' => $iterations, 'unit' => 'iterations'],
        'failures_total' => ['value' => $failures, 'unit' => 'failures'],
        'wall_time_ns_total' => ['value' => $elapsedNs, 'unit' => 'ns'],
        'memory_usage_start_bytes' => ['value' => $memoryStart, 'unit' => 'bytes'],
        'memory_usage_end_bytes' => ['value' => $memoryEnd, 'unit' => 'bytes'],
        'memory_usage_max_bytes' => ['value' => $memoryMax, 'unit' => 'bytes'],
        'memory_usage_delta_bytes' => ['value' => $memoryEnd - $memoryStart, 'unit' => 'bytes'],
        'memory_usage_real_start_bytes' => ['value' => $memoryRealStart, 'unit' => 'bytes'],
        'memory_usage_real_end_bytes' => ['value' => $memoryRealEnd, 'unit' => 'bytes'],
        'memory_usage_real_max_bytes' => ['value' => $memoryRealMax, 'unit' => 'bytes'],
        'memory_usage_real_delta_bytes' => ['value' => $memoryRealEnd - $memoryRealStart, 'unit' => 'bytes'],
        'rss_start_kib' => ['value' => $rssStart, 'unit' => 'KiB'],
        'rss_end_kib' => ['value' => $rssEnd, 'unit' => 'KiB'],
        'rss_max_kib' => ['value' => $rssMax, 'unit' => 'KiB'],
        'rss_max_delta_kib' => ['value' => nullableDelta($rssMax, $rssStart), 'unit' => 'KiB'],
        'fd_count_start' => ['value' => $fdStart, 'unit' => 'fds'],
        'fd_count_end' => ['value' => $fdEnd, 'unit' => 'fds'],
        'fd_count_max' => ['value' => $fdMax, 'unit' => 'fds'],
        'fd_count_delta' => ['value' => nullableDelta($fdEnd, $fdStart), 'unit' => 'fds'],
    ]);
}

function probeUnary(GreeterClient $client): void
{
    [, $status] = $client->BenchUnary(new BenchRequest())->wait();
    if ($status->code !== \Grpc\STATUS_OK) {
        throw new \RuntimeException("probe unary failed: {$status->code}: {$status->details}");
    }
}

function rssKiB(): ?int
{
    $status = @file_get_contents('/proc/self/status');
    if ($status === false) {
        return null;
    }
    if (preg_match('/^VmRSS:\s+(\d+)\s+kB$/m', $status, $matches) !== 1) {
        return null;
    }
    return (int) $matches[1];
}

function fdCount(): ?int
{
    $fds = @scandir('/proc/self/fd');
    if ($fds === false) {
        return null;
    }
    return max(0, count($fds) - 2);
}

function nullableDelta(?int $current, ?int $base): ?int
{
    return $current === null || $base === null ? null : $current - $base;
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/native-lifecycle-stress.php --output=var/bench-results/result.json [--iterations=100] [--message-count=20] [--payload-bytes=1024] [--sleep-us=50000]\n");
    exit(2);
}
