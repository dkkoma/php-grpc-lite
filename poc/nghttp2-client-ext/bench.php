<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

if (!extension_loaded('nghttp2_poc')) {
    fwrite(STDERR, "nghttp2_poc extension is not loaded\n");
    exit(1);
}

$options = getopt('', [
    'iterations::',
    'response-bytes::',
    'request-bytes::',
    'split-grpc-frame',
    'no-copy',
    'data-frame-size::',
    'poll-loop',
]);

$iterations = (int) ($options['iterations'] ?? 1000);
$responseBytes = (int) ($options['response-bytes'] ?? 100);
$requestBytes = (int) ($options['request-bytes'] ?? 0);
$splitGrpcFrame = array_key_exists('split-grpc-frame', $options);
$noCopy = array_key_exists('no-copy', $options);
$dataFrameSize = (int) ($options['data-frame-size'] ?? 0);
$pollLoop = array_key_exists('poll-loop', $options);

$request = new Helloworld\BenchRequest();
$request->setPayloadBytes($responseBytes);
if ($requestBytes > 0) {
    $request->setRequestPayload(str_repeat("\0", $requestBytes));
}

$payload = $request->serializeToString();
$requestBody = $splitGrpcFrame ? $payload : "\0" . pack('N', strlen($payload)) . $payload;

$result = nghttp2_poc_unary_batch('test-server', 50051, '/helloworld.Greeter/BenchUnary', $requestBody, $iterations, [
    'x-bench-server-cached-payload' => '1',
    'x-bench-server-timing' => '1',
    'x-bench-server-stats' => '1',
], $splitGrpcFrame, $noCopy, $dataFrameSize, $pollLoop);

$rawLatencies = $result['latencies_us'];
$latencies = $rawLatencies;
sort($latencies);
$count = count($latencies);
$p50 = percentile($latencies, 0.50);
$p99 = percentile($latencies, 0.99);
$callsPerSecond = $result['total_us'] > 0 ? ($result['ok'] / ($result['total_us'] / 1_000_000)) : 0.0;

$series = [
    'client_first_data_sent_us',
    'client_upload_complete_us',
    'client_response_header_us',
    'client_stream_close_us',
    'server_handler_ns',
    'server_payload_alloc_ns',
    'server_payload_bytes',
    'server_request_payload_bytes',
    'server_stats_handler_start_ns',
    'server_stats_handler_end_ns',
    'server_stats_in_payload_ns',
    'server_stats_out_header_ns',
    'server_stats_out_payload_ns',
    'server_stats_out_payload_bytes',
    'server_stats_out_payload_wire_bytes',
    'server_stats_out_payload_compressed_bytes',
];

foreach ($series as $key) {
    if (!isset($result[$key]) || !is_array($result[$key])) {
        continue;
    }
    $values = $result[$key];
    sort($values);
    $result[$key . '_p50'] = percentile($values, 0.50);
    $result[$key . '_p99'] = percentile($values, 0.99);
}

$result['sample_calls'] = sampleCalls($result, $rawLatencies, min(5, $count));

foreach (array_merge(['latencies_us'], $series) as $key) {
    unset($result[$key]);
}
$result += [
    'response_bytes' => $responseBytes,
    'request_bytes' => $requestBytes,
    'serialized_payload_bytes' => strlen($payload),
    'split_grpc_frame' => $splitGrpcFrame,
    'no_copy' => $noCopy,
    'data_frame_size' => $dataFrameSize,
    'poll_loop' => $pollLoop,
    'p50_us' => $p50,
    'p99_us' => $p99,
    'calls_per_second' => $callsPerSecond,
];

echo json_encode($result, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";

function percentile(array $values, float $percentile): float
{
    $count = count($values);
    if ($count === 0) {
        return 0.0;
    }
    $index = (int) ceil($percentile * $count) - 1;
    $index = max(0, min($count - 1, $index));
    return (float) $values[$index];
}

/**
 * @param array<string, mixed> $result
 * @return list<array<string, int|float>>
 */
function sampleCalls(array $result, array $latencies, int $limit): array
{
    $samples = [];
    for ($i = 0; $i < $limit; $i++) {
        $samples[] = [
            'latency_us' => (int) ($latencies[$i] ?? 0),
            'client_upload_complete_us' => (int) ($result['client_upload_complete_us'][$i] ?? 0),
            'client_response_header_us' => (int) ($result['client_response_header_us'][$i] ?? 0),
            'client_stream_close_us' => (int) ($result['client_stream_close_us'][$i] ?? 0),
            'server_stats_in_payload_ns' => (int) ($result['server_stats_in_payload_ns'][$i] ?? 0),
            'server_stats_out_header_ns' => (int) ($result['server_stats_out_header_ns'][$i] ?? 0),
            'server_stats_out_payload_ns' => (int) ($result['server_stats_out_payload_ns'][$i] ?? 0),
        ];
    }
    return $samples;
}
