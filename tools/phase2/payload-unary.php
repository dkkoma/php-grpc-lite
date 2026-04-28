<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';
require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/UnaryBenchHelper.php';

use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'payload-unary';
$implementation = 'php-grpc-lite';
$output = null;
$target = 'test-server:50051';
$durationSec = 1.0;
$payloadSizes = [0, 100, 1024, 10 * 1024, 100 * 1024];
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
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--duration') {
        $durationSec = (float) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--duration=')) {
        $durationSec = (float) substr($arg, strlen('--duration='));
    } elseif ($arg === '--payload-sizes') {
        $payloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--payload-sizes=')) {
        $payloadSizes = parseIntList(substr($arg, strlen('--payload-sizes=')));
    } elseif ($arg === '--warmup-calls') {
        $warmupCalls = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--warmup-calls=')) {
        $warmupCalls = (int) substr($arg, strlen('--warmup-calls='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $output === null || $output === '') {
    usage('suite, implementation, target, and output are required');
}
if ($durationSec <= 0 || $payloadSizes === [] || $warmupCalls < 0) {
    usage('duration, payload-sizes, and warmup-calls must be valid');
}

$client = UnaryBenchHelper::client($target);
$measurements = [];
foreach ($payloadSizes as $payloadBytes) {
    $request = UnaryBenchHelper::request($payloadBytes);
    for ($warmup = 0; $warmup < $warmupCalls; $warmup++) {
        UnaryBenchHelper::call($client, $request);
    }

    $latenciesNs = [];
    $deadlineNs = (int) round($durationSec * 1_000_000_000);
    $sample = ResourceSampler::measure(static function () use ($client, $request, $deadlineNs, &$latenciesNs): int {
        $startedNs = hrtime(true);
        $calls = 0;
        do {
            $callStartNs = hrtime(true);
            UnaryBenchHelper::call($client, $request);
            $latenciesNs[] = hrtime(true) - $callStartNs;
            $calls++;
        } while (hrtime(true) - $startedNs < $deadlineNs);
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

    $measurements[] = ResultContract::measurement("payload_unary_{$payloadBytes}b", 'payload-unary', 'BenchUnary', [
        'target' => $target,
        'duration_sec' => $durationSec,
        'payload_bytes' => $payloadBytes,
        'warmup_calls' => $warmupCalls,
    ], $metrics);
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-22s %10s %12s %12s %12s\n", 'scenario', 'calls', 'calls/s', 'p50', 'p99');
printf("%'-72s\n", '');
foreach ($measurements as $measurement) {
    printf("%-22s %10d %12.1f %11.1fμs %11.1fμs\n", $measurement['name'], $measurement['metrics']['calls_total']['value'], $measurement['metrics']['calls_per_second']['value'], $measurement['metrics']['latency_p50_ns']['value'] / 1_000, $measurement['metrics']['latency_p99_ns']['value'] / 1_000);
}
echo "JSON: $output\n";

/** @return list<int> */
function parseIntList(string $value): array
{
    $items = [];
    foreach (explode(',', $value) as $part) {
        $number = (int) trim($part);
        if ($number >= 0) {
            $items[] = $number;
        }
    }
    return $items;
}

function writeDocument(string $output, array $document): void
{
    $dir = dirname($output);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
    file_put_contents($output, ResultContract::encode($document));
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/payload-unary.php --suite=payload-unary --implementation=php-grpc-lite --output=var/bench-results/result.json [--duration=1] [--payload-sizes=0,100,1024,10240,102400]\n");
    exit(2);
}
