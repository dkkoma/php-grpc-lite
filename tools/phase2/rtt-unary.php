<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';
require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'rtt-unary';
$implementation = 'php-grpc-lite';
$output = null;
$directTarget = 'test-server:50051';
$toxiproxyAdmin = 'http://toxiproxy:8474';
$payloadBytes = 100;
$serverDelayMs = 0;
$calls = 20;
$warmupCalls = 3;

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
    } elseif ($arg === '--direct-target') {
        $directTarget = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--direct-target=')) {
        $directTarget = substr($arg, strlen('--direct-target='));
    } elseif ($arg === '--toxiproxy-admin') {
        $toxiproxyAdmin = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--toxiproxy-admin=')) {
        $toxiproxyAdmin = substr($arg, strlen('--toxiproxy-admin='));
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
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $directTarget === '' || $toxiproxyAdmin === '') {
    usage('suite, implementation, direct-target, and toxiproxy-admin are required');
}
if ($output === null || $output === '') {
    usage('output is required');
}
if ($payloadBytes < 0 || $serverDelayMs < 0 || $calls <= 0 || $warmupCalls < 0) {
    usage('payload-bytes, server-delay-ms, calls, and warmup-calls must be valid');
}

$proxyCases = [
    ['name' => 'direct', 'target' => $directTarget, 'downstream_latency_ms' => 0, 'proxy' => null],
    ['name' => 'downstream_1ms', 'target' => 'toxiproxy:51051', 'downstream_latency_ms' => 1, 'proxy' => ['name' => 'phase2-rtt-unary-1ms', 'listen' => '0.0.0.0:51051']],
    ['name' => 'downstream_3ms', 'target' => 'toxiproxy:51053', 'downstream_latency_ms' => 3, 'proxy' => ['name' => 'phase2-rtt-unary-3ms', 'listen' => '0.0.0.0:51053']],
    ['name' => 'downstream_5ms', 'target' => 'toxiproxy:51055', 'downstream_latency_ms' => 5, 'proxy' => ['name' => 'phase2-rtt-unary-5ms', 'listen' => '0.0.0.0:51055']],
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

$measurements = [];
foreach ($proxyCases as $proxyCase) {
    $request = UnaryBenchHelper::request($payloadBytes, $serverDelayMs);
    $warmClient = UnaryBenchHelper::client($proxyCase['target']);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($warmClient, $request);
    }

    $measurements[] = runMode(
        'rtt_unary_warm_' . $proxyCase['name'],
        'warm',
        $proxyCase['target'],
        $request,
        $calls,
        static fn () => $warmClient,
        $proxyCase,
        $payloadBytes,
        $serverDelayMs,
    );

    $measurements[] = runMode(
        'rtt_unary_cold_' . $proxyCase['name'],
        'cold',
        $proxyCase['target'],
        $request,
        $calls,
        static fn () => UnaryBenchHelper::client($proxyCase['target']),
        $proxyCase,
        $payloadBytes,
        $serverDelayMs,
    );
}

$document = ResultContract::document($suite, $implementation, $measurements);
$encoded = ResultContract::encode($document);
$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, $encoded);

printf("%-28s %8s %8s %12s %12s %12s\n", 'scenario', 'mode', 'latency', 'p50', 'p95', 'p99');
printf("%'-86s\n", '');
foreach ($measurements as $measurement) {
    printf(
        "%-28s %8s %7dms %11.1fμs %11.1fμs %11.1fμs\n",
        $measurement['name'],
        $measurement['attributes']->mode,
        $measurement['attributes']->toxiproxy_downstream_latency_ms,
        $measurement['metrics']['latency_p50_ns']['value'] / 1_000,
        $measurement['metrics']['latency_p95_ns']['value'] / 1_000,
        $measurement['metrics']['latency_p99_ns']['value'] / 1_000,
    );
}
echo "JSON: $output\n";

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
): array {
    $latenciesNs = [];
    $sample = ResourceSampler::measure(static function () use ($clientFactory, $request, $calls, &$latenciesNs): int {
        for ($call = 0; $call < $calls; $call++) {
            $client = $clientFactory();
            $startedNs = hrtime(true);
            UnaryBenchHelper::call($client, $request);
            $latenciesNs[] = hrtime(true) - $startedNs;
            unset($client);
        }

        return $calls;
    });

    $metrics = $sample['metrics'];
    $percentiles = UnaryBenchHelper::percentiles($latenciesNs);
    $metrics['calls_total'] = [
        'value' => $calls,
        'unit' => 'calls',
    ];
    $metrics['wall_time_ns_per_call'] = [
        'value' => $metrics['wall_time_ns_total']['value'] / $calls,
        'unit' => 'ns/call',
    ];
    $metrics['diagnostic_cpu_total_us_per_call'] = [
        'value' => $metrics['diagnostic_cpu_total_us_total']['value'] / $calls,
        'unit' => 'us/call',
    ];
    foreach ($percentiles as $percentile => $value) {
        $metrics['latency_' . $percentile . '_ns'] = [
            'value' => $value,
            'unit' => 'ns',
        ];
    }

    return ResultContract::measurement(
        $name,
        'rtt',
        'BenchUnary',
        [
            'mode' => $mode,
            'target' => $target,
            'calls' => $calls,
            'payload_bytes' => $payloadBytes,
            'server_delay_ms' => $serverDelayMs,
            'toxiproxy_downstream_latency_ms' => $proxyCase['downstream_latency_ms'],
            'latency_model' => $proxyCase['proxy'] === null ? 'direct' : 'toxiproxy downstream latency only',
        ],
        $metrics,
    );
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
        "Usage: php tools/phase2/rtt-unary.php --suite=rtt-unary --implementation=php-grpc-lite --output=var/bench-results/result.json [--calls=20] [--payload-bytes=100]\n",
    );
    exit(2);
}
