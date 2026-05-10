<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';

use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;

$args = $argv;
array_shift($args);

$suite = 'contract-smoke';
$implementation = 'php-grpc-lite';
$output = null;
$autoload = null;
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
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? null;
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
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

$sample = ResourceSampler::measure(static function () use ($revs): int {
    $accumulator = 0;
    for ($iteration = 0; $iteration < $revs; $iteration++) {
        $accumulator += $iteration & 1;
    }

    return $accumulator;
});
$metrics = $sample['metrics'];
$metrics['wall_time_ns_per_op'] = [
    'value' => $metrics['wall_time_ns_total']['value'] / $revs,
    'unit' => 'ns/op',
];
$metrics['ops_per_second'] = [
    'value' => $revs / ($metrics['wall_time_ns_total']['value'] / 1_000_000_000),
    'unit' => 'ops/s',
];
$metrics['accumulator'] = [
    'value' => $sample['result'],
    'unit' => null,
];

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
                'purpose' => 'exercise Phase 2 result JSON without network',
            ],
            $metrics,
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
printf("%-32s %12d %11.1fns\n", 'contract_smoke_loop', $revs, $metrics['wall_time_ns_per_op']['value']);
echo "JSON: $output\n";

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/contract-smoke.php --suite=contract-smoke --implementation=php-grpc-lite --output=var/bench-results/result.json [--revs=100000]\n");
    exit(2);
}
