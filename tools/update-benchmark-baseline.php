<?php
declare(strict_types=1);

/**
 * Update the explicit regression baseline from parsed PHPBench JSON.
 *
 * This tool is intentionally narrow: it updates one suite/implementation pair
 * from one current run. Historical benchmark records live under docs.
 */

$args = $argv;
array_shift($args);

$baselinePath = null;
$currentPath = null;
$suite = null;
$implementation = 'php-grpc-lite';
$source = null;

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
    } elseif ($arg === '--source') {
        $source = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--source=')) {
        $source = substr($arg, strlen('--source='));
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

if (!isset($baseline['entries']) || !is_array($baseline['entries'])) {
    fwrite(STDERR, "baseline entries must be an array\n");
    exit(2);
}
if (!is_array($currentRows) || $currentRows === []) {
    fwrite(STDERR, "current rows must be a non-empty array\n");
    exit(2);
}

$source ??= $currentPath;
$baseline['updated_at'] = date('Y-m-d');
$updated = 0;
$added = 0;

foreach ($currentRows as $row) {
    if (!is_array($row)) {
        fwrite(STDERR, "current rows must contain objects\n");
        exit(2);
    }

    $entry = [
        'suite' => $suite,
        'implementation' => $implementation,
        'benchmark' => requireString($row, 'benchmark'),
        'subject' => requireString($row, 'subject'),
        'set' => $row['set'] ?? null,
        'mode_ns' => requireInt($row, 'mode_ns'),
        'mem_peak_bytes' => requireInt($row, 'mem_peak_bytes'),
        'source' => $source,
    ];

    $index = findEntryIndex($baseline['entries'], $entry);
    if ($index === null) {
        $baseline['entries'][] = $entry;
        $added++;
        continue;
    }

    $baseline['entries'][$index] = array_merge(
        $baseline['entries'][$index],
        [
            'mode_ns' => $entry['mode_ns'],
            'mem_peak_bytes' => $entry['mem_peak_bytes'],
            'source' => $entry['source'],
        ],
    );
    $updated++;
}

file_put_contents($baselinePath, encodeJson($baseline));
printf("updated=%d added=%d baseline=%s\n", $updated, $added, $baselinePath);

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

/** @param array<string, mixed> $data */
function encodeJson(array $data): string
{
    return json_encode($data, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n";
}

/**
 * @param list<array<string, mixed>> $entries
 * @param array<string, mixed> $needle
 */
function findEntryIndex(array $entries, array $needle): ?int
{
    foreach ($entries as $index => $entry) {
        if (
            ($entry['suite'] ?? null) === $needle['suite']
            && ($entry['implementation'] ?? null) === $needle['implementation']
            && ($entry['benchmark'] ?? null) === $needle['benchmark']
            && ($entry['subject'] ?? null) === $needle['subject']
            && ($entry['set'] ?? null) === $needle['set']
        ) {
            return $index;
        }
    }

    return null;
}

/** @param array<string, mixed> $row */
function requireString(array $row, string $key): string
{
    if (!isset($row[$key]) || !is_string($row[$key]) || $row[$key] === '') {
        fwrite(STDERR, "row is missing string field: $key\n");
        exit(2);
    }
    return $row[$key];
}

/** @param array<string, mixed> $row */
function requireInt(array $row, string $key): int
{
    if (!isset($row[$key]) || !is_int($row[$key])) {
        fwrite(STDERR, "row is missing integer field: $key\n");
        exit(2);
    }
    return $row[$key];
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(
        STDERR,
        "Usage: php tools/update-benchmark-baseline.php --baseline=bench/baselines/regression.json --current=var/bench-results/result.json --suite=cold [--implementation=php-grpc-lite] [--source=label]\n",
    );
    exit(2);
}
