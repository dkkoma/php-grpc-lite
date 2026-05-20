<?php
declare(strict_types=1);

require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/BenchTelemetry.php';
require __DIR__ . '/RpcGap.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Benchmark\BenchTelemetry;
use PhpGrpcLite\Tools\Benchmark\ResourceSampler;
use PhpGrpcLite\Tools\Benchmark\RpcGap;
use PhpGrpcLite\Tools\Benchmark\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'rtt-unary';
$implementation = 'php-grpc-lite';
$directTarget = 'test-server:50051';
$toxiproxyAdmin = 'http://toxiproxy:8474';
$autoload = 'vendor/autoload.php';
$payloadBytes = 100;
$serverDelayMs = 0;
$calls = 20;
$warmupCalls = 3;
$rpcGapMs = RpcGap::fromEnvironment();

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
    } elseif ($arg === '--direct-target') {
        $directTarget = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--direct-target=')) {
        $directTarget = substr($arg, strlen('--direct-target='));
    } elseif ($arg === '--toxiproxy-admin') {
        $toxiproxyAdmin = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--toxiproxy-admin=')) {
        $toxiproxyAdmin = substr($arg, strlen('--toxiproxy-admin='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--server-delay-ms') {
        $serverDelayMs = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--server-delay-ms=')) {
        $serverDelayMs = (int) substr($arg, strlen('--server-delay-ms='));
    } elseif ($arg === '--calls') {
        $calls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--calls=')) {
        $calls = (int) substr($arg, strlen('--calls='));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } elseif (RpcGap::consumeArgument($arg, $args, $argIndex, $rpcGapMs)) {
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $directTarget === '' || $toxiproxyAdmin === '' || $autoload === '') {
    usage('suite, implementation, direct-target, toxiproxy-admin, and autoload are required');
}
if ($payloadBytes < 0 || $serverDelayMs < 0 || $calls <= 0 || $warmupCalls < 0) {
    usage('payload-bytes, server-delay-ms, calls, and warmup-calls must be valid');
}

requireAutoload($autoload);
$benchTelemetry = BenchTelemetry::requiredFromEnvironment($suite, $implementation);
register_shutdown_function([$benchTelemetry, 'shutdown']);

$proxyCases = [
    ['name' => 'direct', 'target' => $directTarget, 'downstream_latency_ms' => 0, 'proxy' => null],
    ['name' => 'downstream_1ms', 'target' => 'toxiproxy:51051', 'downstream_latency_ms' => 1, 'proxy' => ['name' => 'benchmark-rtt-unary-1ms', 'listen' => '0.0.0.0:51051']],
    ['name' => 'downstream_3ms', 'target' => 'toxiproxy:51053', 'downstream_latency_ms' => 3, 'proxy' => ['name' => 'benchmark-rtt-unary-3ms', 'listen' => '0.0.0.0:51053']],
    ['name' => 'downstream_5ms', 'target' => 'toxiproxy:51055', 'downstream_latency_ms' => 5, 'proxy' => ['name' => 'benchmark-rtt-unary-5ms', 'listen' => '0.0.0.0:51055']],
];

foreach ($proxyCases as $proxyCase) {
    if ($proxyCase['proxy'] === null) {
        continue;
    }
    configureProxy(
        $toxiproxyAdmin,
        $proxyCase['proxy']['name'],
        $proxyCase['proxy']['listen'],
        $directTarget,
        $proxyCase['downstream_latency_ms'],
    );
}

foreach ($proxyCases as $proxyCase) {
    $request = UnaryBenchHelper::request($payloadBytes, $serverDelayMs);
    $warmClient = UnaryBenchHelper::client($proxyCase['target']);
    $benchTelemetry->setContext('rtt_unary_warm_' . $proxyCase['name'], [
        'benchmark.mode' => 'warm',
        'benchmark.target' => $proxyCase['target'],
        'benchmark.payload_bytes' => $payloadBytes,
        'benchmark.server_delay_ms' => $serverDelayMs,
        'benchmark.toxiproxy_downstream_latency_ms' => $proxyCase['downstream_latency_ms'],
    ]);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($warmClient, $request);
    }

    runMode(
        'rtt_unary_warm_' . $proxyCase['name'],
        'warm',
        $proxyCase['target'],
        $request,
        $calls,
        static fn () => $warmClient,
        $proxyCase,
        $payloadBytes,
        $serverDelayMs,
        $rpcGapMs,
        $benchTelemetry,
    );

    runMode(
        'rtt_unary_cold_' . $proxyCase['name'],
        'cold',
        $proxyCase['target'],
        $request,
        $calls,
        static fn () => UnaryBenchHelper::client($proxyCase['target']),
        $proxyCase,
        $payloadBytes,
        $serverDelayMs,
        $rpcGapMs,
        $benchTelemetry,
    );
}


echo "OTEL spans exported.\n";

/**
 * @param array{name: string, target: string, downstream_latency_ms: int, proxy: mixed} $proxyCase
 * @return array<string, mixed>
 */
function runMode(
    string $name,
    string $mode,
    string $target,
    BenchRequest $request,
    int $calls,
    callable $clientFactory,
    array $proxyCase,
    int $payloadBytes,
    int $serverDelayMs,
    int $rpcGapMs,
    ?BenchTelemetry $benchTelemetry,
): void {
    $latenciesNs = [];
    $benchTelemetry->setContext($name, [
        'benchmark.mode' => $mode,
        'benchmark.target' => $target,
        'benchmark.payload_bytes' => $payloadBytes,
        'benchmark.server_delay_ms' => $serverDelayMs,
        'benchmark.toxiproxy_downstream_latency_ms' => $proxyCase['downstream_latency_ms'],
        'benchmark.rpc_gap_ms' => $rpcGapMs,
    ]);
    $sample = ResourceSampler::measure(static function () use ($clientFactory, $request, $calls, $rpcGapMs, $benchTelemetry, &$latenciesNs): int {
        for ($call = 0; $call < $calls; $call++) {
            $client = $clientFactory();
            $startedNs = hrtime(true);
            UnaryBenchHelper::call($client, $request);
            $callEndNs = hrtime(true);
            $benchTelemetry->recordRpcSpan('BenchUnary', $startedNs, $callEndNs, [
                'rpc.service' => 'helloworld.Greeter',
                'rpc.method' => 'BenchUnary',
            ]);
            $latenciesNs[] = $callEndNs - $startedNs;
            unset($client);
            RpcGap::sleepBetweenCalls($rpcGapMs, $call + 1 < $calls);
        }

        return $calls;
    });

}

function configureProxy(string $adminUrl, string $name, string $listen, string $upstream, int $latencyMs): void
{
    toxiproxyRequest('DELETE', "$adminUrl/proxies/" . rawurlencode($name));
    toxiproxyRequest('POST', "$adminUrl/proxies", [
        'name' => $name,
        'listen' => $listen,
        'upstream' => $upstream,
        'enabled' => true,
    ], [200, 201, 409]);
    toxiproxyRequest('POST', "$adminUrl/proxies/" . rawurlencode($name) . '/toxics', [
        'name' => 'downstream_latency',
        'type' => 'latency',
        'stream' => 'downstream',
        'toxicity' => 1.0,
        'attributes' => [
            'latency' => $latencyMs,
            'jitter' => 0,
        ],
    ], [200, 201, 409]);
}

/**
 * @param array<string, mixed>|null $body
 * @param list<int> $okStatuses
 */
function toxiproxyRequest(string $method, string $url, ?array $body = null, array $okStatuses = [200, 204, 404]): void
{
    $curl = curl_init($url);
    if ($curl === false) {
        throw new \RuntimeException('failed to initialize curl');
    }
    curl_setopt($curl, CURLOPT_CUSTOMREQUEST, $method);
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
    if ($body !== null) {
        curl_setopt($curl, CURLOPT_HTTPHEADER, ['content-type: application/json']);
        curl_setopt($curl, CURLOPT_POSTFIELDS, json_encode($body, JSON_THROW_ON_ERROR));
    }
    $response = curl_exec($curl);
    $status = curl_getinfo($curl, CURLINFO_RESPONSE_CODE);
    $error = curl_error($curl);
    curl_close($curl);

    if (!in_array($status, $okStatuses, true)) {
        throw new \RuntimeException("Toxiproxy $method $url failed: HTTP $status $error $response");
    }
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(
        STDERR,
        "Usage: php tools/benchmark/rtt-unary.php --suite=rtt-unary --implementation=php-grpc-lite [--calls=20] [--payload-bytes=100] [--rpc-gap-ms=10]\n",
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
