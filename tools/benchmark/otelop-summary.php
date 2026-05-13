<?php
declare(strict_types=1);

$endpoint = getenv('BENCH_OTEL_UI_ENDPOINT') ?: 'http://otelop:4319/graphql';
$runId = getenv('BENCH_OTEL_RUN_ID') ?: '';
$suite = getenv('BENCH_OTEL_SUMMARY_SUITE') ?: '';
$limit = (int) (getenv('BENCH_OTEL_SUMMARY_LIMIT') ?: '100000');

$args = $argv;
array_shift($args);
for ($index = 0; $index < count($args); $index++) {
    $arg = $args[$index];
    if ($arg === '--endpoint') {
        $endpoint = $args[++$index] ?? '';
    } elseif (str_starts_with($arg, '--endpoint=')) {
        $endpoint = substr($arg, strlen('--endpoint='));
    } elseif ($arg === '--run-id') {
        $runId = $args[++$index] ?? '';
    } elseif (str_starts_with($arg, '--run-id=')) {
        $runId = substr($arg, strlen('--run-id='));
    } elseif ($arg === '--suite') {
        $suite = $args[++$index] ?? '';
    } elseif (str_starts_with($arg, '--suite=')) {
        $suite = substr($arg, strlen('--suite='));
    } elseif ($arg === '--limit') {
        $limit = (int) ($args[++$index] ?? 0);
    } elseif (str_starts_with($arg, '--limit=')) {
        $limit = (int) substr($arg, strlen('--limit='));
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($endpoint === '' || $runId === '' || $limit <= 0) {
    usage('endpoint, run-id, and limit are required');
}

$query = <<<'GRAPHQL'
query Traces($limit: Int!, $offset: Int!) {
  traces(limit: $limit, offset: $offset) {
    total
    items {
      rootSpan {
        name
        durationMs
        statusCode
        attributes
      }
    }
  }
}
GRAPHQL;

$groups = [];
for ($offset = 0; ; $offset += min($limit, 1000)) {
    $pageLimit = min($limit, 1000);
    $response = postJson($endpoint, [
        'query' => $query,
        'variables' => ['limit' => $pageLimit, 'offset' => $offset],
    ]);
    $connection = $response['data']['traces'] ?? [];
    $items = $connection['items'] ?? [];
    if (!is_array($items) || $items === []) {
        break;
    }

    foreach ($items as $item) {
        $span = $item['rootSpan'] ?? null;
        if (!is_array($span)) {
            continue;
        }
        $attributes = $span['attributes'] ?? [];
        if (!is_array($attributes)) {
            continue;
        }
        if (($attributes['benchmark.run_id'] ?? '') !== $runId) {
            continue;
        }
        if ($suite !== '' && ($attributes['benchmark.suite'] ?? '') !== $suite) {
            continue;
        }

        $shape = shapeKey($attributes);
        $keyParts = [
            (string) ($attributes['benchmark.suite'] ?? ''),
            (string) ($attributes['benchmark.measurement'] ?? ''),
            $shape,
            (string) ($attributes['benchmark.implementation'] ?? ''),
            (string) ($attributes['benchmark.transport'] ?? '-'),
        ];
        $key = implode("\t", $keyParts);
        $groups[$key]['suite'] = $keyParts[0];
        $groups[$key]['measurement'] = $keyParts[1];
        $groups[$key]['shape'] = $shape;
        $groups[$key]['implementation'] = $keyParts[3];
        $groups[$key]['transport'] = $keyParts[4];
        $groups[$key]['durations_us'][] = ((float) ($span['durationMs'] ?? 0.0)) * 1000.0;
    }

    $total = (int) ($connection['total'] ?? 0);
    if ($offset + $pageLimit >= $total || $offset + $pageLimit >= $limit) {
        break;
    }
}

ksort($groups);
printf(
    "%-28s %-24s %-18s %-14s %8s %12s %12s %12s\n",
    'suite',
    'measurement',
    'shape',
    'variant',
    'count',
    'span_p50_us',
    'span_p99_us',
    'span_max_us',
);
printf("%'-125s\n", '');
foreach ($groups as $group) {
    $durations = $group['durations_us'];
    $spanPercentiles = percentiles($durations);
    printf(
        "%-28s %-24s %-18s %-14s %8d %12.1f %12.1f %12.1f\n",
        $group['suite'],
        $group['measurement'],
        $group['shape'],
        variantName($group['implementation'], $group['transport']),
        count($durations),
        $spanPercentiles['p50'],
        $spanPercentiles['p99'],
        $spanPercentiles['max'],
    );
}

/** @param array<string, mixed> $attributes */
function shapeKey(array $attributes): string
{
    if (isset($attributes['benchmark.payload_bytes'])) {
        return 'payload=' . (string) $attributes['benchmark.payload_bytes'];
    }
    if (isset($attributes['benchmark.request_bytes']) || isset($attributes['benchmark.response_bytes'])) {
        return 'req=' . (string) ($attributes['benchmark.request_bytes'] ?? '') . ',resp=' . (string) ($attributes['benchmark.response_bytes'] ?? '');
    }
    if (isset($attributes['benchmark.request_payload_bytes'])) {
        return 'request_payload=' . (string) $attributes['benchmark.request_payload_bytes'];
    }
    if (isset($attributes['benchmark.operation_shape'])) {
        return (string) $attributes['benchmark.operation_shape'];
    }
    return '-';
}

/** @return array<string, mixed> */
function postJson(string $endpoint, array $payload): array
{
    $context = stream_context_create([
        'http' => [
            'method' => 'POST',
            'header' => "Content-Type: application/json\r\n",
            'content' => json_encode($payload, JSON_UNESCAPED_SLASHES),
            'timeout' => 30.0,
            'ignore_errors' => true,
        ],
    ]);
    $body = file_get_contents($endpoint, false, $context);
    if ($body === false) {
        throw new RuntimeException("failed to query otelop endpoint: $endpoint");
    }
    $decoded = json_decode($body, true);
    if (!is_array($decoded)) {
        throw new RuntimeException('otelop returned non-JSON response');
    }
    if (isset($decoded['errors'])) {
        throw new RuntimeException('otelop GraphQL error: ' . json_encode($decoded['errors'], JSON_UNESCAPED_SLASHES));
    }
    return $decoded;
}

function variantName(string $implementation, string $transport): string
{
    if ($implementation === 'php-grpc-lite' && $transport !== '-' && $transport !== '') {
        return $transport;
    }

    return $implementation;
}

/** @param list<float> $values */
function percentiles(array $values): array
{
    sort($values, SORT_NUMERIC);
    return [
        'p50' => percentile($values, 50.0),
        'p99' => percentile($values, 99.0),
        'max' => $values[count($values) - 1],
    ];
}

/** @param list<float> $sortedValues */
function percentile(array $sortedValues, float $percentile): float
{
    $index = (int) ceil(($percentile / 100.0) * count($sortedValues)) - 1;
    $index = max(0, min(count($sortedValues) - 1, $index));
    return $sortedValues[$index];
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/benchmark/otelop-summary.php --run-id=<BENCH_OTEL_RUN_ID> [--suite=spanner-dml-unary-shape] [--endpoint=http://otelop:4319/graphql] [--limit=100000]\n");
    exit(2);
}
