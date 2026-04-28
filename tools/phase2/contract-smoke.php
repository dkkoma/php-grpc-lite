<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';

use PhpGrpcLite\Tools\Phase2\ResultContract;

$args = $argv;
array_shift($args);

$suite = 'contract-smoke';
$implementation = 'php-grpc-lite';
$output = null;
$revs = 100_000;

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
    } elseif ($arg === '--revs') {
        $revs = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--revs=')) {
        $revs = (int) substr($arg, strlen('--revs='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '') {
    usage('suite and implementation are required');
}
if ($output === null || $output === '') {
    usage('output is required');
}
if ($revs <= 0) {
    usage('revs must be greater than zero');
}

$memoryBefore = memory_get_usage(true);
$peakBefore = memory_get_peak_usage(true);
$start = hrtime(true);

$accumulator = 0;
for ($iteration = 0; $iteration < $revs; $iteration++) {
    $accumulator += $iteration & 1;
}

$elapsedNs = hrtime(true) - $start;
$memoryAfter = memory_get_usage(true);
$peakAfter = memory_get_peak_usage(true);

$document = ResultContract::document(
    $suite,
    $implementation,
    [
        ResultContract::measurement(
            'contract_smoke_loop',
            'contract',
            'integer_loop',
            [
                'revs' => $revs,
                'purpose' => 'exercise Phase 2 result JSON without network or PHPBench',
            ],
            [
                'wall_time_ns_total' => [
                    'value' => $elapsedNs,
                    'unit' => 'ns',
                ],
                'wall_time_ns_per_op' => [
                    'value' => $elapsedNs / $revs,
                    'unit' => 'ns/op',
                ],
                'ops_per_second' => [
                    'value' => $revs / ($elapsedNs / 1_000_000_000),
                    'unit' => 'ops/s',
                ],
                'memory_usage_delta_bytes' => [
                    'value' => $memoryAfter - $memoryBefore,
                    'unit' => 'bytes',
                ],
                'memory_peak_delta_bytes' => [
                    'value' => $peakAfter - $peakBefore,
                    'unit' => 'bytes',
                ],
                'accumulator' => [
                    'value' => $accumulator,
                    'unit' => null,
                ],
            ],
        ),
    ],
);

$encoded = ResultContract::encode($document);
$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, $encoded);

printf("%-32s %12s %12s\n", 'measurement', 'revs', 'avg');
printf("%'-60s\n", '');
printf("%-32s %12d %11.1fns\n", 'contract_smoke_loop', $revs, $elapsedNs / $revs);
echo "JSON: $output\n";

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/contract-smoke.php --suite=contract-smoke --implementation=php-grpc-lite --output=var/bench-results/result.json [--revs=100000]\n");
    exit(2);
}
