<?php
declare(strict_types=1);

/**
 * Compare parsed PHPBench JSON rows against a regression baseline.
 *
 * Baselines are intentionally explicit and updated by commit. They are not a
 * historical record; docs/benchmarks/*.md keeps that role.
 */

$args = $argv;
array_shift($args);

$baselinePath = null;
$currentPath = null;
$suite = null;
$implementation = null;

for ($i = 0; $i < count($args); $i++) {
    $arg = $args[$i];
    if ($arg === '--baseline') {
        $baselinePath = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--baseline=')) {
        $baselinePath = substr($arg, strlen('--baseline='));
    } elseif ($arg === '--current') {
        $currentPath = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--current=')) {
        $currentPath = substr($arg, strlen('--current='));
    } elseif ($arg === '--suite') {
        $suite = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--suite=')) {
        $suite = substr($arg, strlen('--suite='));
    } elseif ($arg === '--implementation') {
        $implementation = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--implementation=')) {
        $implementation = substr($arg, strlen('--implementation='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($baselinePath === null || !is_file($baselinePath)) {
    usage('baseline file is required');
}
if ($currentPath === null || !is_file($currentPath)) {
    usage('current parsed JSON file is required');
}
if ($suite === null || $suite === '') {
    usage('suite is required');
}
if ($implementation === null || $implementation === '') {
    usage('implementation is required');
}

$baseline = decodeJsonFile($baselinePath);
$currentRows = decodeJsonFile($currentPath);

$defaultThresholds = $baseline['default_thresholds'] ?? [];
$entries = $baseline['entries'] ?? [];
if (!is_array($entries)) {
    fwrite(STDERR, "baseline entries must be an array\n");
    exit(2);
}

$baselineByKey = [];
foreach ($entries as $entry) {
    if (($entry['suite'] ?? null) !== $suite || ($entry['implementation'] ?? null) !== $implementation) {
        continue;
    }
    $baselineByKey[rowKey($entry)] = $entry;
}

$failures = 0;
$warnings = 0;
$matched = 0;

printf(
    "%-62s %12s %12s %9s %9s %8s\n",
    'metric',
    'baseline',
    'current',
    'mode Δ',
    'mem Δ',
    'status',
);
printf("%'-118s\n", '');

foreach ($currentRows as $row) {
    $key = rowKey($row);
    if (!isset($baselineByKey[$key])) {
        $warnings++;
        printf("%-62s %12s %12s %9s %9s %8s\n", metricLabel($row), '-', formatNs((int) $row['mode_ns']), '-', '-', 'missing');
        continue;
    }

    $matched++;
    $base = $baselineByKey[$key];
    $thresholds = array_merge($defaultThresholds, $base['thresholds'] ?? []);
    $modeDelta = percentDelta((int) $base['mode_ns'], (int) $row['mode_ns']);
    $memDelta = null;
    if (isset($base['mem_peak_bytes'], $row['mem_peak_bytes'])) {
        $memDelta = percentDelta((int) $base['mem_peak_bytes'], (int) $row['mem_peak_bytes']);
    }
    $rstdev = isset($row['rstdev_percent']) ? (float) $row['rstdev_percent'] : null;
    $status = 'ok';

    if ($modeDelta > (float) ($thresholds['mode_fail_percent'] ?? 25.0)) {
        $status = 'fail';
        $failures++;
    } elseif ($modeDelta > (float) ($thresholds['mode_warn_percent'] ?? 15.0)) {
        $status = 'warn';
        $warnings++;
    }

    if ($memDelta !== null) {
        if ($memDelta > (float) ($thresholds['mem_peak_fail_percent'] ?? INF)) {
            if ($status !== 'fail') {
                $failures++;
            }
            $status = 'fail';
        } elseif ($memDelta > (float) ($thresholds['mem_peak_warn_percent'] ?? INF) && $status === 'ok') {
            $status = 'warn';
            $warnings++;
        }
    }

    $maxRstdev = $thresholds['max_rstdev_percent'] ?? null;
    if ($maxRstdev !== null && $rstdev !== null && $rstdev > (float) $maxRstdev) {
        if ($status === 'ok') {
            $status = 'noisy';
            $warnings++;
        }
    }

    printf(
        "%-62s %12s %12s %+8.2f%% %8s %8s\n",
        metricLabel($row),
        formatNs((int) $base['mode_ns']),
        formatNs((int) $row['mode_ns']),
        $modeDelta,
        $memDelta === null ? '-' : sprintf('%+7.2f%%', $memDelta),
        $status,
    );
}

printf("\nmatched=%d warnings=%d failures=%d\n", $matched, $warnings, $failures);

if ($failures > 0) {
    exit(1);
}
if ($matched === 0) {
    exit(2);
}

/** @return array<string, mixed> */
function decodeJsonFile(string $path): array
{
    $decoded = json_decode((string) file_get_contents($path), true);
    if (!is_array($decoded)) {
        fwrite(STDERR, "failed to decode JSON: $path\n");
        exit(2);
    }
    return $decoded;
}

/** @param array<string, mixed> $row */
function rowKey(array $row): string
{
    return implode("\t", [
        (string) ($row['benchmark'] ?? ''),
        (string) ($row['subject'] ?? ''),
        (string) ($row['set'] ?? ''),
    ]);
}

/** @param array<string, mixed> $row */
function metricLabel(array $row): string
{
    $set = $row['set'] ?? null;
    return $row['benchmark'] . '/' . $row['subject'] . ($set === null || $set === '' ? '' : '#' . $set);
}

function percentDelta(int $baseline, int $current): float
{
    if ($baseline <= 0) {
        return 0.0;
    }
    return (($current - $baseline) / $baseline) * 100.0;
}

function formatNs(int $ns): string
{
    if ($ns >= 1_000_000) {
        return sprintf('%.3fms', $ns / 1_000_000);
    }
    if ($ns >= 1_000) {
        return sprintf('%.3fμs', $ns / 1_000);
    }
    return $ns . 'ns';
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(
        STDERR,
        "Usage: php tools/compare-benchmark-baseline.php --baseline=bench/baselines/regression.json --current=var/bench-results/result.json --suite=cold --implementation=php-grpc-lite\n",
    );
    exit(2);
}
