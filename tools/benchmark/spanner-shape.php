<?php
declare(strict_types=1);

require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/RpcGap.php';
require __DIR__ . '/StreamingBenchHelper.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use Grpc\ChannelCredentials;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\RpcGap;
use PhpGrpcLite\Tools\Benchmark\StreamingBenchHelper;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'spanner-shape';
$implementation = 'php-grpc-lite';
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$calls = 1000;
$warmupCalls = 10;
$nativeResponseMode = 'stream';
$transport = 'native';
$tlsRoot = '';
$rpcGapMs = RpcGap::fromEnvironment();

$unaryCases = [
    ['name' => 'begin_txn_unary', 'request_bytes' => 92, 'response_bytes' => 18],
    ['name' => 'commit_txn_unary', 'request_bytes' => 106, 'response_bytes' => 14],
];
$streamingCases = [
    ['name' => 'select_1row_10col_streaming', 'request_bytes' => 160, 'response_bytes' => 100, 'message_count' => 1],
    ['name' => 'dml_insert_10col_streaming', 'request_bytes' => 355, 'response_bytes' => 8, 'message_count' => 1],
    ['name' => 'dml_update_10col_streaming', 'request_bytes' => 327, 'response_bytes' => 8, 'message_count' => 1],
    ['name' => 'dml_delete_10col_streaming', 'request_bytes' => 144, 'response_bytes' => 8, 'message_count' => 1],
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
    } elseif ($arg === '--tls-root') {
        $tlsRoot = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--tls-root=')) {
        $tlsRoot = substr($arg, strlen('--tls-root='));
    } elseif (RpcGap::consumeArgument($arg, $args, $argIndex, $rpcGapMs)) {
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === 'tls-spanner-shape' && $target === 'test-server:50051') {
    $target = 'test-server:50052';
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
if ($implementation === 'php-grpc-lite' && $transport === 'franken-go') {
    $clientOptions['grpc_lite.backend'] = 'franken-go';
}
$clientOptions += tlsClientOptions($suite, $tlsRoot);
$unaryClient = UnaryBenchHelper::client($target, $clientOptions);
$streamingClient = StreamingBenchHelper::client($target, $clientOptions);

foreach ($unaryCases as $case) {
    $request = unaryRequest($case['request_bytes'], $case['response_bytes']);
    $benchTelemetry->setContext($case['name'], commonContext($target, $calls, $warmupCalls, $transport, isTlsSuite($suite)) + [
        'benchmark.call_type' => 'unary',
        'benchmark.request_bytes' => $case['request_bytes'],
        'benchmark.response_bytes' => $case['response_bytes'],
        'benchmark.operation_shape' => $case['name'],
        'benchmark.rpc_gap_ms' => $rpcGapMs,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($unaryClient, $request);
    }
    for ($call = 0; $call < $calls; $call++) {
        $startNs = hrtime(true);
        $statusCode = 1;
        try {
            UnaryBenchHelper::call($unaryClient, $request);
        } catch (Throwable $throwable) {
            $statusCode = 2;
            throw $throwable;
        } finally {
            $endNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchUnary', $startNs, $endNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchUnary',
            ], $statusCode);
            RpcGap::sleepBetweenCalls($rpcGapMs, $call + 1 < $calls);
        }
    }
}

foreach ($streamingCases as $case) {
    $request = streamingRequest($case['message_count'], $case['request_bytes'], $case['response_bytes']);
    $benchTelemetry->setContext($case['name'], commonContext($target, $calls, $warmupCalls, $transport, isTlsSuite($suite)) + [
        'benchmark.call_type' => 'server_streaming',
        'benchmark.request_bytes' => $case['request_bytes'],
        'benchmark.response_bytes' => $case['response_bytes'],
        'benchmark.message_count' => $case['message_count'],
        'benchmark.native_response_mode' => $nativeResponseMode,
        'benchmark.operation_shape' => $case['name'],
        'benchmark.rpc_gap_ms' => $rpcGapMs,
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        StreamingBenchHelper::drain($streamingClient, $request);
    }
    for ($call = 0; $call < $calls; $call++) {
        $startNs = hrtime(true);
        $statusCode = 1;
        try {
            StreamingBenchHelper::drain($streamingClient, $request);
        } catch (Throwable $throwable) {
            $statusCode = 2;
            throw $throwable;
        } finally {
            $endNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchServerStream', $startNs, $endNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchServerStream',
            ], $statusCode);
            RpcGap::sleepBetweenCalls($rpcGapMs, $call + 1 < $calls);
        }
    }
}

echo "OTEL spans exported.\n";

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
function commonContext(string $target, int $calls, int $warmupCalls, string $transport, bool $tls): array
{
    return [
        'benchmark.target' => $target,
        'benchmark.calls' => $calls,
        'benchmark.warmup_calls' => $warmupCalls,
        'benchmark.transport' => $transport,
        'benchmark.security' => $tls ? 'tls' : 'h2c',
        'benchmark.spanner_path' => 'synthetic-shape',
    ];
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

function isTlsSuite(string $suite): bool
{
    return str_starts_with($suite, 'tls-');
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/spanner-shape.php --suite=spanner-shape|tls-spanner-shape --implementation=php-grpc-lite [--calls=1000] [--warmup-calls=10] [--tls-root=...] [--rpc-gap-ms=10]\n");
    exit(2);
}
