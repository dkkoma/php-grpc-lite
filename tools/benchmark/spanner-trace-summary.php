<?php

declare(strict_types=1);

if ($argc < 2) {
    fwrite(STDERR, "Usage: php tools/benchmark/spanner-trace-summary.php <trace.ndjson>\n");
    exit(2);
}

$path = $argv[1];
$handle = fopen($path, 'rb');
if ($handle === false) {
    fwrite(STDERR, "failed to open trace file: {$path}\n");
    exit(1);
}

$steps = [];
$rpcs = [];
$rpcResponses = [];
$grpcLiteRpcs = [];
$events = [];

while (($line = fgets($handle)) !== false) {
    $line = trim($line);
    if ($line === '') {
        continue;
    }
    $record = json_decode($line, true);
    if (!is_array($record)) {
        continue;
    }
    $event = (string) ($record['event'] ?? '');
    $events[$event] = ($events[$event] ?? 0) + 1;

    if ($event === 'step.end' && isset($record['step'], $record['elapsed_us'])) {
        $steps[(string) $record['step']][] = (int) $record['elapsed_us'];
    }
    if ($event === 'rpc.request') {
        $name = (string) ($record['rpc_name'] ?? 'unknown');
        $rpcs[$name] = ($rpcs[$name] ?? 0) + 1;
    }
    if ($event === 'rpc.response' && isset($record['latency_ms'])) {
        $name = (string) ($record['rpc_name'] ?? 'unknown');
        $rpcResponses[$name][] = (int) $record['latency_ms'];
    }
    if ($event === 'rpc.end' && isset($record['elapsed_us'])) {
        $name = (string) ($record['rpc_method'] ?? 'unknown');
        $grpcLiteRpcs[$name][] = (int) $record['elapsed_us'];
    }
}

fclose($handle);

ksort($events);
ksort($steps);
ksort($rpcs);
ksort($rpcResponses);
ksort($grpcLiteRpcs);

echo "# Events\n";
foreach ($events as $event => $count) {
    echo $event . "\t" . $count . PHP_EOL;
}

echo PHP_EOL . "# Step elapsed_us\n";
foreach ($steps as $step => $values) {
    printStats($step, $values);
}

echo PHP_EOL . "# RPC request count\n";
foreach ($rpcs as $rpc => $count) {
    echo $rpc . "\t" . $count . PHP_EOL;
}

echo PHP_EOL . "# RPC response latency_ms\n";
foreach ($rpcResponses as $rpc => $values) {
    printStats($rpc, $values);
}

echo PHP_EOL . "# grpc-lite RPC elapsed_us\n";
foreach ($grpcLiteRpcs as $rpc => $values) {
    printStats($rpc, $values);
}

/**
 * @param list<int> $values
 */
function printStats(string $name, array $values): void
{
    sort($values);
    $count = count($values);
    if ($count === 0) {
        return;
    }
    $sum = array_sum($values);
    printf(
        "%s\tn=%d\tavg=%.3f\tp50=%d\tp90=%d\tp95=%d\tp99=%d\tmax=%d\n",
        $name,
        $count,
        $sum / $count,
        percentile($values, 0.50),
        percentile($values, 0.90),
        percentile($values, 0.95),
        percentile($values, 0.99),
        $values[$count - 1],
    );
}

/**
 * @param list<int> $values
 */
function percentile(array $values, float $percentile): int
{
    $count = count($values);
    if ($count === 0) {
        return 0;
    }
    $index = (int) ceil($count * $percentile) - 1;
    $index = max(0, min($count - 1, $index));
    return $values[$index];
}
