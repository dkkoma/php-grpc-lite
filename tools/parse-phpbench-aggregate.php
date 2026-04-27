<?php
declare(strict_types=1);

/**
 * Parse PHPBench's aggregate table from a saved log into machine-readable
 * JSON or TSV. This intentionally consumes the text report so benchmark runs
 * can keep using the human-friendly aggregate output.
 */

$args = $argv;
array_shift($args);

$format = 'json';
$output = null;
$input = null;

for ($i = 0; $i < count($args); $i++) {
    $arg = $args[$i];
    if ($arg === '--format') {
        $format = $args[++$i] ?? '';
    } elseif (str_starts_with($arg, '--format=')) {
        $format = substr($arg, strlen('--format='));
    } elseif ($arg === '--output') {
        $output = $args[++$i] ?? null;
    } elseif (str_starts_with($arg, '--output=')) {
        $output = substr($arg, strlen('--output='));
    } elseif ($input === null) {
        $input = $arg;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($input === null || !is_file($input)) {
    usage('input log file is required');
}
if (!in_array($format, ['json', 'tsv'], true)) {
    usage('format must be json or tsv');
}

$rows = parseAggregate(file_get_contents($input), $input);
if ($rows === []) {
    fwrite(STDERR, "no PHPBench aggregate rows found in $input\n");
    exit(1);
}

$encoded = $format === 'json' ? encodeJson($rows) : encodeTsv($rows);

if ($output !== null) {
    $dir = dirname($output);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
    file_put_contents($output, $encoded);
} else {
    echo $encoded;
}

/**
 * @return list<array<string, int|float|string|null>>
 */
function parseAggregate(string $log, string $source): array
{
    $rows = [];
    $log = preg_replace('/\x1b\[[0-9;?]*[A-Za-z]/', '', $log) ?? $log;
    $log = str_replace("\r", "\n", $log);

    foreach (explode("\n", $log) as $line) {
        $line = trim($line);
        if (!str_starts_with($line, '|') || !str_ends_with($line, '|')) {
            continue;
        }

        $cols = array_map('trim', explode('|', trim($line, '|')));
        if (count($cols) !== 8 || $cols[0] === 'benchmark') {
            continue;
        }

        [$benchmark, $subject, $set, $revs, $iterations, $memPeak, $mode, $rstdev] = $cols;
        if ($benchmark === '' || $subject === '') {
            continue;
        }

        $rows[] = [
            'source' => $source,
            'benchmark' => $benchmark,
            'subject' => $subject,
            'set' => $set === '' ? null : $set,
            'revs' => (int) $revs,
            'iterations' => (int) $iterations,
            'mem_peak' => $memPeak,
            'mem_peak_bytes' => parseBytes($memPeak),
            'mode' => $mode,
            'mode_ns' => parseDurationNs($mode),
            'rstdev' => $rstdev,
            'rstdev_percent' => parsePercent($rstdev),
        ];
    }

    return $rows;
}

/** @param list<array<string, int|float|string|null>> $rows */
function encodeJson(array $rows): string
{
    return json_encode($rows, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n";
}

/** @param list<array<string, int|float|string|null>> $rows */
function encodeTsv(array $rows): string
{
    $headers = [
        'source',
        'benchmark',
        'subject',
        'set',
        'revs',
        'iterations',
        'mem_peak',
        'mem_peak_bytes',
        'mode',
        'mode_ns',
        'rstdev',
        'rstdev_percent',
    ];
    $lines = [implode("\t", $headers)];
    foreach ($rows as $row) {
        $fields = [];
        foreach ($headers as $header) {
            $value = $row[$header] ?? '';
            $fields[] = $value === null ? '' : (string) $value;
        }
        $lines[] = implode("\t", $fields);
    }
    return implode("\n", $lines) . "\n";
}

function parseDurationNs(string $value): ?int
{
    if (!preg_match('/^([0-9]+(?:\.[0-9]+)?)\s*([a-zμ]+)$/iu', trim($value), $m)) {
        return null;
    }

    $number = (float) $m[1];
    $unit = strtolower($m[2]);
    $multiplier = match ($unit) {
        'ns' => 1,
        'μs', 'us' => 1_000,
        'ms' => 1_000_000,
        's' => 1_000_000_000,
        default => null,
    };

    return $multiplier === null ? null : (int) round($number * $multiplier);
}

function parseBytes(string $value): ?int
{
    if (!preg_match('/^([0-9]+(?:\.[0-9]+)?)\s*([kmgt]?b)$/i', trim($value), $m)) {
        return null;
    }

    $number = (float) $m[1];
    $unit = strtolower($m[2]);
    $multiplier = match ($unit) {
        'b' => 1,
        'kb' => 1024,
        'mb' => 1024 ** 2,
        'gb' => 1024 ** 3,
        'tb' => 1024 ** 4,
        default => null,
    };

    return $multiplier === null ? null : (int) round($number * $multiplier);
}

function parsePercent(string $value): ?float
{
    $normalized = str_replace(['±', '%'], '', trim($value));
    return is_numeric($normalized) ? (float) $normalized : null;
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/parse-phpbench-aggregate.php [--format=json|tsv] [--output=path] <phpbench.log>\n");
    exit(2);
}
