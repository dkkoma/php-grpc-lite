<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchReply;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$args = $argv;
array_shift($args);

$suite = 'payload-breakdown';
$implementation = 'php-grpc-lite';
$output = null;
$autoload = 'vendor/autoload.php';
$payloadSizes = [0, 100, 1024, 10 * 1024, 100 * 1024];
$revsOverride = null;

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
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--payload-sizes') {
        $payloadSizes = parseIntList($args[++$argIndex] ?? '');
    } elseif (str_starts_with($arg, '--payload-sizes=')) {
        $payloadSizes = parseIntList(substr($arg, strlen('--payload-sizes=')));
    } elseif ($arg === '--revs') {
        $revsOverride = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--revs=')) {
        $revsOverride = (int) substr($arg, strlen('--revs='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, autoload, and output are required');
}
if ($payloadSizes === [] || ($revsOverride !== null && $revsOverride <= 0)) {
    usage('payload-sizes and revs must be valid');
}

requireAutoload($autoload);
gc_disable();

$measurements = [];
foreach ($payloadSizes as $payloadBytes) {
    $reply = new BenchReply();
    $reply->setPayload(str_repeat("\0", $payloadBytes));
    $payload = $reply->serializeToString();
    $frame = "\x00" . pack('N', strlen($payload)) . $payload;
    $payloadLength = strlen($payload);
    $frameLength = strlen($frame);
    $revs = $revsOverride ?? defaultRevs($payloadBytes);

    $measurements[] = measureCase(
        "payload_breakdown_frame_length_{$payloadBytes}b",
        'frame-length',
        $payloadBytes,
        $payloadLength,
        $frameLength,
        $revs,
        static function () use ($frame): int {
            return unpack('N', substr($frame, 1, 4))[1];
        },
    );

    $measurements[] = measureCase(
        "payload_breakdown_payload_slice_{$payloadBytes}b",
        'payload-slice',
        $payloadBytes,
        $payloadLength,
        $frameLength,
        $revs,
        static function () use ($frame): int {
            $length = unpack('N', substr($frame, 1, 4))[1];
            return strlen(substr($frame, 5, $length));
        },
    );

    $measurements[] = measureCase(
        "payload_breakdown_decode_only_{$payloadBytes}b",
        'protobuf-decode',
        $payloadBytes,
        $payloadLength,
        $frameLength,
        $revs,
        static function () use ($payload): int {
            $message = new BenchReply();
            $message->mergeFromString($payload);
            return strlen($message->getPayload());
        },
    );

    $measurements[] = measureCase(
        "payload_breakdown_slice_decode_{$payloadBytes}b",
        'payload-slice+protobuf-decode',
        $payloadBytes,
        $payloadLength,
        $frameLength,
        $revs,
        static function () use ($frame): int {
            $length = unpack('N', substr($frame, 1, 4))[1];
            $payload = substr($frame, 5, $length);
            $message = new BenchReply();
            $message->mergeFromString($payload);
            return strlen($message->getPayload());
        },
    );

    $measurements[] = measureCase(
        "payload_breakdown_deserialize_apply_{$payloadBytes}b",
        'deserialize-apply',
        $payloadBytes,
        $payloadLength,
        $frameLength,
        $revs,
        static function () use ($payload): int {
            $message = \Grpc\Internal\Deserialize::apply([BenchReply::class, 'decode'], $payload);
            return strlen($message->getPayload());
        },
    );
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-46s %8s %12s %12s %12s\n", 'scenario', 'revs', 'p50', 'p95', 'p99');
printf("%'-96s\n", '');
foreach ($measurements as $measurement) {
    printf(
        "%-46s %8d %11.1fμs %11.1fμs %11.1fμs\n",
        $measurement['name'],
        $measurement['metrics']['operations_total']['value'],
        $measurement['metrics']['latency_p50_ns']['value'] / 1_000,
        $measurement['metrics']['latency_p95_ns']['value'] / 1_000,
        $measurement['metrics']['latency_p99_ns']['value'] / 1_000,
    );
}
echo "JSON: $output\n";

/**
 * @param callable(): int $subject
 * @return array<string, mixed>
 */
function measureCase(
    string $name,
    string $operation,
    int $payloadBytes,
    int $protobufBytes,
    int $frameBytes,
    int $revs,
    callable $subject,
): array {
    for ($warmup = 0; $warmup < min(1000, $revs); $warmup++) {
        $subject();
    }

    $latenciesNs = [];
    $sample = ResourceSampler::measure(static function () use ($subject, $revs, &$latenciesNs): int {
        $checksum = 0;
        for ($rev = 0; $rev < $revs; $rev++) {
            $startedNs = hrtime(true);
            $checksum += $subject();
            $latenciesNs[] = hrtime(true) - $startedNs;
        }
        return $checksum;
    });

    $metrics = $sample['metrics'];
    $metrics['operations_total'] = ['value' => $revs, 'unit' => 'operations'];
    $metrics['operations_per_second'] = [
        'value' => $revs / ($metrics['wall_time_ns_total']['value'] / 1_000_000_000),
        'unit' => 'operations/s',
    ];
    $metrics['wall_time_ns_per_operation'] = [
        'value' => $metrics['wall_time_ns_total']['value'] / $revs,
        'unit' => 'ns/operation',
    ];
    foreach (UnaryBenchHelper::percentiles($latenciesNs) as $percentile => $value) {
        $metrics['latency_' . $percentile . '_ns'] = ['value' => $value, 'unit' => 'ns'];
    }
    $metrics['diagnostic_checksum'] = ['value' => $sample['result'], 'unit' => 'bytes'];

    return ResultContract::measurement($name, 'payload-breakdown', $operation, [
        'payload_bytes' => $payloadBytes,
        'protobuf_bytes' => $protobufBytes,
        'grpc_frame_bytes' => $frameBytes,
        'revs' => $revs,
        'gc_enabled' => gc_enabled(),
    ], $metrics);
}

function defaultRevs(int $payloadBytes): int
{
    if ($payloadBytes >= 100 * 1024) {
        return 1000;
    }
    if ($payloadBytes >= 10 * 1024) {
        return 5000;
    }
    return 20_000;
}

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

/**
 * @param array<string, mixed> $document
 */
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
    fwrite(STDERR, "Usage: php tools/phase2/payload-breakdown.php --suite=payload-breakdown --implementation=php-grpc-lite --output=var/bench-results/result.json [--payload-sizes=0,100,1024,10240,102400] [--revs=1000]\n");
    exit(2);
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload file not found: $autoload");
    }
    require $autoload;
}
