<?php
if ($argc < 3) {
    fwrite(STDERR, "Usage: php wire-summary.php TRACE_JSONL MARKERS_LOG\n");
    exit(64);
}

$tracePath = $argv[1];
$markersPath = $argv[2];

$streams = [];
if (is_file($tracePath)) {
    $fh = fopen($tracePath, 'rb');
    while (($line = fgets($fh)) !== false) {
        $event = json_decode($line, true);
        if (!is_array($event) || !isset($event['event'], $event['stream_id'], $event['monotonic_us'])) {
            continue;
        }
        $streamId = (int)$event['stream_id'];
        if ($streamId <= 0 || ($streamId % 2) === 0) {
            continue;
        }
        $stream =& $streams[$streamId];
        if (!isset($stream)) {
            $stream = [
                'method' => $event['rpc_method'] ?? '',
                'headers_out_us' => null,
                'headers_payload_len' => null,
                'data_out_us' => null,
                'first_in_us' => null,
                'last_in_us' => null,
            ];
        }
        if (($event['rpc_method'] ?? '') !== '') {
            $stream['method'] = $event['rpc_method'];
        }
        if ($event['event'] === 'wire.frame_out' && ($event['frame_type'] ?? '') === 'HEADERS' && $stream['headers_out_us'] === null) {
            $stream['headers_out_us'] = (int)$event['monotonic_us'];
            $stream['headers_payload_len'] = (int)($event['header_block_len'] ?? $event['frame_payload_len'] ?? 0);
        }
        if ($event['event'] === 'wire.frame_out' && ($event['frame_type'] ?? '') === 'DATA' && $stream['data_out_us'] === null) {
            $stream['data_out_us'] = (int)$event['monotonic_us'];
        }
        if ($event['event'] === 'wire.frame_in') {
            if ($stream['first_in_us'] === null) {
                $stream['first_in_us'] = (int)$event['monotonic_us'];
            }
            $stream['last_in_us'] = (int)$event['monotonic_us'];
        }
        unset($stream);
    }
    fclose($fh);
}

$hasMethod = false;
foreach ($streams as $stream) {
    if (($stream['method'] ?? '') !== '') {
        $hasMethod = true;
        break;
    }
}
if (!$hasMethod && count($streams) >= 3) {
    $streamIds = array_keys($streams);
    sort($streamIds, SORT_NUMERIC);
    $lastStreamId = $streamIds[count($streamIds) - 1];
    foreach ($streamIds as $streamId) {
        if ($streamId === $streamIds[0]) {
            $streams[$streamId]['method'] = '/google.spanner.v1.Spanner/CreateSession';
        } elseif ($streamId === $lastStreamId) {
            $streams[$streamId]['method'] = '/google.spanner.v1.Spanner/DeleteSession';
        } else {
            $streams[$streamId]['method'] = '/google.spanner.v1.Spanner/ExecuteStreamingSql';
        }
    }
}

$markers = [];
if (is_file($markersPath)) {
    foreach (file($markersPath, FILE_IGNORE_NEW_LINES) as $line) {
        if (preg_match('/^MARK end iter=(\d+) .* elapsed_us=(\d+)/', $line, $m)) {
            $markers[] = (int)$m[2];
        }
    }
}

$selectStreams = [];
foreach ($streams as $streamId => $stream) {
    if (($stream['method'] ?? '') !== '/google.spanner.v1.Spanner/ExecuteStreamingSql') {
        continue;
    }
    if ($stream['headers_out_us'] === null || $stream['first_in_us'] === null) {
        continue;
    }
    $selectStreams[] = [
        'stream_id' => $streamId,
        'headers_payload_len' => $stream['headers_payload_len'],
        'headers_to_first_in_us' => $stream['first_in_us'] - $stream['headers_out_us'],
        'headers_to_last_in_us' => $stream['last_in_us'] !== null ? $stream['last_in_us'] - $stream['headers_out_us'] : null,
    ];
}

$percentile = static function (array $values, float $p): ?int {
    if ($values === []) {
        return null;
    }
    sort($values);
    $idx = (int)min(count($values) - 1, floor(count($values) * $p));
    return $values[$idx];
};

$steadyStreams = array_values(array_filter($selectStreams, static fn($stream) => $stream['stream_id'] >= 5));
$firstLatencies = array_column($selectStreams, 'headers_to_first_in_us');
$lastLatencies = array_values(array_filter(array_column($selectStreams, 'headers_to_last_in_us'), static fn($v) => $v !== null));
$headerSizes = array_column($selectStreams, 'headers_payload_len');
$steadyFirstLatencies = array_column($steadyStreams, 'headers_to_first_in_us');
$steadyLastLatencies = array_values(array_filter(array_column($steadyStreams, 'headers_to_last_in_us'), static fn($v) => $v !== null));
$steadyHeaderSizes = array_column($steadyStreams, 'headers_payload_len');

printf("trace=%s\n", $tracePath);
printf("markers=%s\n", $markersPath);
printf("select_streams=%d\n", count($selectStreams));
printf("steady_select_streams=%d\n", count($steadyStreams));
printf("header_payload_len_unique=%s\n", implode(',', array_values(array_unique($headerSizes))));
printf("steady_header_payload_len_unique=%s\n", implode(',', array_values(array_unique($steadyHeaderSizes))));
printf("headers_to_first_in_us_p50=%s\n", $percentile($firstLatencies, 0.50) ?? 'n/a');
printf("headers_to_first_in_us_p90=%s\n", $percentile($firstLatencies, 0.90) ?? 'n/a');
printf("headers_to_first_in_us_p99=%s\n", $percentile($firstLatencies, 0.99) ?? 'n/a');
printf("headers_to_last_in_us_p50=%s\n", $percentile($lastLatencies, 0.50) ?? 'n/a');
printf("steady_headers_to_first_in_us_p50=%s\n", $percentile($steadyFirstLatencies, 0.50) ?? 'n/a');
printf("steady_headers_to_first_in_us_p90=%s\n", $percentile($steadyFirstLatencies, 0.90) ?? 'n/a');
printf("steady_headers_to_first_in_us_p99=%s\n", $percentile($steadyFirstLatencies, 0.99) ?? 'n/a');
printf("steady_headers_to_last_in_us_p50=%s\n", $percentile($steadyLastLatencies, 0.50) ?? 'n/a');
printf("marker_elapsed_us_p50=%s\n", $percentile($markers, 0.50) ?? 'n/a');
printf("marker_elapsed_us_p90=%s\n", $percentile($markers, 0.90) ?? 'n/a');
printf("marker_elapsed_us_p99=%s\n", $percentile($markers, 0.99) ?? 'n/a');

echo "\nstreams\n";
foreach ($selectStreams as $stream) {
    printf(
        "stream=%d header_payload=%d first_us=%d last_us=%s\n",
        $stream['stream_id'],
        $stream['headers_payload_len'],
        $stream['headers_to_first_in_us'],
        $stream['headers_to_last_in_us'] ?? 'n/a'
    );
}
