<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

if (!(extension_loaded('grpc'))) {
    fwrite(STDERR, "grpc extension is not loaded\n");
    exit(1);
}

$options = getopt('', [
    'iterations::',
    'response-bytes::',
    'request-bytes::',
    'rpc::',
    'message-count::',
    'split-grpc-frame',
    'no-copy',
    'data-frame-size::',
    'recv-stream-window-size::',
    'recv-connection-window-size::',
    'recv-buffer-size::',
    'flush-after-mem-recv',
    'read-first-poll-loop',
    'decode-response-messages',
    'response-callback-mode::',
    'incremental-decode',
    'compact-response-buffer',
    'response-compact-threshold::',
    'direct-response-payload',
    'read-ahead-delivery',
    'read-ahead-max-messages::',
    'read-ahead-max-bytes::',
    'poll-loop',
    'discard-response-body',
]);

$iterations = (int) ($options['iterations'] ?? 1000);
$responseBytes = (int) ($options['response-bytes'] ?? 100);
$requestBytes = (int) ($options['request-bytes'] ?? 0);
$rpc = (string) ($options['rpc'] ?? 'unary');
$messageCount = (int) ($options['message-count'] ?? 1);
$splitGrpcFrame = array_key_exists('split-grpc-frame', $options);
$noCopy = array_key_exists('no-copy', $options);
$dataFrameSize = (int) ($options['data-frame-size'] ?? 0);
$recvStreamWindowSize = (int) ($options['recv-stream-window-size'] ?? 0);
$recvConnectionWindowSize = (int) ($options['recv-connection-window-size'] ?? 0);
$recvBufferSize = (int) ($options['recv-buffer-size'] ?? 16384);
$flushAfterMemRecv = array_key_exists('flush-after-mem-recv', $options);
$readFirstPollLoop = array_key_exists('read-first-poll-loop', $options);
$decodeResponseMessages = array_key_exists('decode-response-messages', $options);
$responseCallbackMode = (string) ($options['response-callback-mode'] ?? ($decodeResponseMessages ? 'decode-yield' : 'none'));
$incrementalDecode = array_key_exists('incremental-decode', $options);
$compactResponseBuffer = array_key_exists('compact-response-buffer', $options);
$responseCompactThreshold = (int) ($options['response-compact-threshold'] ?? 1);
$directResponsePayload = array_key_exists('direct-response-payload', $options);
$readAheadDelivery = array_key_exists('read-ahead-delivery', $options);
$readAheadMaxMessages = (int) ($options['read-ahead-max-messages'] ?? 0);
$readAheadMaxBytes = (int) ($options['read-ahead-max-bytes'] ?? 0);
$pollLoop = array_key_exists('poll-loop', $options);
$discardResponseBody = array_key_exists('discard-response-body', $options);
if ($responseCallbackMode !== 'none') {
    $discardResponseBody = false;
}

if (!in_array($rpc, ['unary', 'server-stream'], true)) {
    fwrite(STDERR, "--rpc must be unary or server-stream\n");
    exit(2);
}
if ($messageCount < 1) {
    fwrite(STDERR, "--message-count must be positive\n");
    exit(2);
}
if (!in_array($responseCallbackMode, ['none', 'noop', 'strlen', 'decode', 'decode-yield'], true)) {
    fwrite(STDERR, "--response-callback-mode must be none, noop, strlen, decode, or decode-yield\n");
    exit(2);
}

$request = new Helloworld\BenchRequest();
$request->setPayloadBytes($responseBytes);
if ($rpc === 'server-stream') {
    $request->setMessageCount($messageCount);
}
if ($requestBytes > 0) {
    $request->setRequestPayload(str_repeat("\0", $requestBytes));
}

$payload = $request->serializeToString();
$requestBody = $splitGrpcFrame ? $payload : "\0" . pack('N', strlen($payload)) . $payload;
$path = $rpc === 'server-stream' ? '/helloworld.Greeter/BenchServerStream' : '/helloworld.Greeter/BenchUnary';

if (!function_exists('grpc_lite_bench_unary_batch')) {
    fwrite(STDERR, "grpc_lite_bench_unary_batch is not available. Rebuild ext/grpc with PHP_GRPC_LITE_ENABLE_BENCH for this diagnostic script.\n");
    exit(2);
}

$responseCallback = match ($responseCallbackMode) {
    'noop' => static fn (string $payload): null => null,
    'strlen' => static fn (string $payload): int => strlen($payload),
    'decode' => static fn (string $payload): Helloworld\BenchReply => decodeBenchReply($payload),
    'decode-yield' => static fn (string $payload): Helloworld\BenchReply => decodeAndYieldBenchReply($payload),
    default => null,
};

$result = grpc_lite_bench_unary_batch('test-server', 50051, $path, $requestBody, $iterations, [
    'x-bench-server-cached-payload' => '1',
    'x-bench-server-timing' => '1',
    'x-bench-server-stats' => '1',
], $splitGrpcFrame, $noCopy, $dataFrameSize, $pollLoop, $discardResponseBody, $recvStreamWindowSize, $recvConnectionWindowSize, $recvBufferSize, $flushAfterMemRecv, $readFirstPollLoop, $responseCallback, $incrementalDecode, $compactResponseBuffer, $responseCompactThreshold, $directResponsePayload, $readAheadDelivery, $readAheadMaxMessages, $readAheadMaxBytes, 0);

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
    'client_first_response_data_us',
    'client_last_response_data_us',
    'client_first_window_update_us',
    'client_last_window_update_us',
    'client_first_window_update_sent_us',
    'client_last_window_update_sent_us',
    'client_first_flow_control_pause_us',
    'client_response_header_us',
    'client_stream_close_us',
    'client_first_response_message_ready_us',
    'client_last_response_message_ready_us',
    'client_first_response_callback_done_us',
    'client_last_response_callback_done_us',
    'call_window_update_frames_recv',
    'call_connection_window_update_frames_recv',
    'call_stream_window_update_frames_recv',
    'call_connection_window_update_increment_recv',
    'call_stream_window_update_increment_recv',
    'call_window_update_frames_sent',
    'call_connection_window_update_frames_sent',
    'call_stream_window_update_frames_sent',
    'call_connection_window_update_increment_sent',
    'call_stream_window_update_increment_sent',
    'call_data_read_length_calls',
    'call_flow_control_pauses',
    'call_max_write_syscall_us',
    'call_recv_syscalls',
    'call_recv_syscall_us',
    'call_max_recv_syscall_us',
    'call_mem_recv_us',
    'call_max_mem_recv_us',
    'call_session_send_after_recv_us',
    'call_max_session_send_after_recv_us',
    'call_poll_wait_us',
    'call_max_poll_wait_us',
    'call_pollin_ready',
    'call_pollout_ready',
    'call_poll_to_data_us',
    'call_max_poll_to_data_us',
    'call_window_update_to_data_us',
    'call_max_window_update_to_data_us',
    'call_receive_drains',
    'call_receive_drains_with_data',
    'call_receive_drains_eagain_after_data',
    'call_max_reads_per_drain',
    'call_max_bytes_per_drain',
    'call_min_session_remote_window',
    'call_min_stream_remote_window',
    'call_response_data_bytes',
    'call_data_recv_calls',
    'call_body_append_us',
    'call_max_body_append_us',
    'call_body_compact_count',
    'call_body_compact_bytes',
    'call_body_compact_us',
    'call_max_body_compact_us',
    'call_max_body_buffer_bytes',
    'call_decoded_messages',
    'call_max_response_queue_count',
    'call_max_response_queue_bytes',
    'call_response_queue_wait_us',
    'call_max_response_queue_wait_us',
    'call_response_payload_string_us',
    'call_max_response_payload_string_us',
    'call_response_decode_us',
    'call_max_response_decode_us',
    'server_handler_ns',
    'server_payload_alloc_ns',
    'server_payload_bytes',
    'server_request_payload_bytes',
    'server_stats_handler_start_ns',
    'server_stats_handler_end_ns',
    'server_stats_in_payload_ns',
    'server_stats_out_header_ns',
    'server_stats_out_payload_ns',
    'server_stats_first_out_payload_ns',
    'server_stats_last_out_payload_ns',
    'server_stats_out_payload_count',
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

$derivedSeries = buildDerivedSeries($result, $rawLatencies);
foreach ($derivedSeries as $key => $values) {
    $sorted = $values;
    sort($sorted);
    $result[$key . '_p50'] = percentile($sorted, 0.50);
    $result[$key . '_p99'] = percentile($sorted, 0.99);
}

$result['sample_calls'] = sampleCalls($result, $rawLatencies, min(5, $count));
$result['tail_sample_calls'] = tailSampleCalls($result, $rawLatencies, min(5, $count), $derivedSeries);

foreach (array_merge(['latencies_us'], $series) as $key) {
    unset($result[$key]);
}
$result += [
    'response_bytes' => $responseBytes,
    'request_bytes' => $requestBytes,
    'rpc' => $rpc,
    'message_count' => $messageCount,
    'serialized_payload_bytes' => strlen($payload),
    'split_grpc_frame' => $splitGrpcFrame,
    'no_copy' => $noCopy,
    'data_frame_size' => $dataFrameSize,
    'recv_stream_window_size' => $recvStreamWindowSize,
    'recv_connection_window_size' => $recvConnectionWindowSize,
    'recv_buffer_size' => $recvBufferSize,
    'flush_after_mem_recv' => $flushAfterMemRecv,
    'read_first_poll_loop' => $readFirstPollLoop,
    'decode_response_messages' => $responseCallbackMode === 'decode-yield',
    'response_callback_mode' => $responseCallbackMode,
    'incremental_decode' => $incrementalDecode,
    'compact_response_buffer' => $compactResponseBuffer,
    'response_compact_threshold' => $responseCompactThreshold,
    'direct_response_payload' => $directResponsePayload,
    'read_ahead_delivery' => $readAheadDelivery,
    'read_ahead_max_messages' => $readAheadMaxMessages,
    'read_ahead_max_bytes' => $readAheadMaxBytes,
    'poll_loop' => $pollLoop,
    'discard_response_body' => $discardResponseBody,
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

function decodeAndYieldBenchReply(string $payload): Helloworld\BenchReply
{
    $reply = decodeBenchReply($payload);
    foreach (yieldBenchReply($reply) as $yielded) {
        return $yielded;
    }

    throw new \RuntimeException('yieldBenchReply did not yield');
}

function decodeBenchReply(string $payload): Helloworld\BenchReply
{
    $reply = new Helloworld\BenchReply();
    $reply->mergeFromString($payload);

    return $reply;
}

/** @return \Generator<int, Helloworld\BenchReply> */
function yieldBenchReply(Helloworld\BenchReply $reply): \Generator
{
    yield $reply;
}

/**
 * @param array<string, mixed> $result
 * @param list<int> $latencies
 * @return array<string, list<int>>
 */
function buildDerivedSeries(array $result, array $latencies): array
{
    $series = [
        'call_loop_active_us' => [],
        'call_loop_known_including_poll_us' => [],
        'call_unaccounted_after_loop_known_us' => [],
        'call_mem_recv_inner_known_us' => [],
        'call_after_last_data_to_close_us' => [],
        'call_after_message_ready_to_close_us' => [],
        'call_after_callback_done_to_close_us' => [],
        'call_server_last_to_close_us' => [],
    ];

    foreach ($latencies as $index => $latency) {
        $loopActive =
            (int) ($result['call_recv_syscall_us'][$index] ?? 0)
            + (int) ($result['call_mem_recv_us'][$index] ?? 0)
            + (int) ($result['call_session_send_after_recv_us'][$index] ?? 0);
        $memRecvInnerKnown =
            + (int) ($result['call_body_append_us'][$index] ?? 0)
            + (int) ($result['call_body_compact_us'][$index] ?? 0)
            + (int) ($result['call_response_payload_string_us'][$index] ?? 0)
            + (int) ($result['call_response_decode_us'][$index] ?? 0);
        $knownIncludingPoll = $loopActive + (int) ($result['call_poll_wait_us'][$index] ?? 0);
        $streamClose = (int) ($result['client_stream_close_us'][$index] ?? 0);
        $lastData = (int) ($result['client_last_response_data_us'][$index] ?? 0);
        $messageReady = (int) ($result['client_last_response_message_ready_us'][$index] ?? 0);
        $callbackDone = (int) ($result['client_last_response_callback_done_us'][$index] ?? 0);
        $serverLast = (int) round(((int) ($result['server_stats_last_out_payload_ns'][$index] ?? 0)) / 1000);

        $series['call_loop_active_us'][] = $loopActive;
        $series['call_loop_known_including_poll_us'][] = $knownIncludingPoll;
        $series['call_unaccounted_after_loop_known_us'][] = max(0, (int) $latency - $knownIncludingPoll);
        $series['call_mem_recv_inner_known_us'][] = $memRecvInnerKnown;
        $series['call_after_last_data_to_close_us'][] = max(0, $streamClose - $lastData);
        $series['call_after_message_ready_to_close_us'][] = $messageReady > 0 ? max(0, $streamClose - $messageReady) : 0;
        $series['call_after_callback_done_to_close_us'][] = $callbackDone > 0 ? max(0, $streamClose - $callbackDone) : 0;
        $series['call_server_last_to_close_us'][] = $serverLast > 0 ? max(0, $streamClose - $serverLast) : 0;
    }

    return $series;
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
            'client_first_response_data_us' => (int) ($result['client_first_response_data_us'][$i] ?? 0),
            'client_last_response_data_us' => (int) ($result['client_last_response_data_us'][$i] ?? 0),
            'client_first_window_update_us' => (int) ($result['client_first_window_update_us'][$i] ?? 0),
            'client_last_window_update_us' => (int) ($result['client_last_window_update_us'][$i] ?? 0),
            'client_first_window_update_sent_us' => (int) ($result['client_first_window_update_sent_us'][$i] ?? 0),
            'client_last_window_update_sent_us' => (int) ($result['client_last_window_update_sent_us'][$i] ?? 0),
            'client_first_flow_control_pause_us' => (int) ($result['client_first_flow_control_pause_us'][$i] ?? 0),
            'client_response_header_us' => (int) ($result['client_response_header_us'][$i] ?? 0),
            'client_stream_close_us' => (int) ($result['client_stream_close_us'][$i] ?? 0),
            'call_window_update_frames_recv' => (int) ($result['call_window_update_frames_recv'][$i] ?? 0),
            'call_connection_window_update_frames_recv' => (int) ($result['call_connection_window_update_frames_recv'][$i] ?? 0),
            'call_stream_window_update_frames_recv' => (int) ($result['call_stream_window_update_frames_recv'][$i] ?? 0),
            'call_connection_window_update_increment_recv' => (int) ($result['call_connection_window_update_increment_recv'][$i] ?? 0),
            'call_stream_window_update_increment_recv' => (int) ($result['call_stream_window_update_increment_recv'][$i] ?? 0),
            'call_window_update_frames_sent' => (int) ($result['call_window_update_frames_sent'][$i] ?? 0),
            'call_connection_window_update_frames_sent' => (int) ($result['call_connection_window_update_frames_sent'][$i] ?? 0),
            'call_stream_window_update_frames_sent' => (int) ($result['call_stream_window_update_frames_sent'][$i] ?? 0),
            'call_connection_window_update_increment_sent' => (int) ($result['call_connection_window_update_increment_sent'][$i] ?? 0),
            'call_stream_window_update_increment_sent' => (int) ($result['call_stream_window_update_increment_sent'][$i] ?? 0),
            'call_data_read_length_calls' => (int) ($result['call_data_read_length_calls'][$i] ?? 0),
            'call_flow_control_pauses' => (int) ($result['call_flow_control_pauses'][$i] ?? 0),
            'call_max_write_syscall_us' => (int) ($result['call_max_write_syscall_us'][$i] ?? 0),
            'call_recv_syscalls' => (int) ($result['call_recv_syscalls'][$i] ?? 0),
            'call_recv_syscall_us' => (int) ($result['call_recv_syscall_us'][$i] ?? 0),
            'call_max_recv_syscall_us' => (int) ($result['call_max_recv_syscall_us'][$i] ?? 0),
            'call_mem_recv_us' => (int) ($result['call_mem_recv_us'][$i] ?? 0),
            'call_max_mem_recv_us' => (int) ($result['call_max_mem_recv_us'][$i] ?? 0),
            'call_session_send_after_recv_us' => (int) ($result['call_session_send_after_recv_us'][$i] ?? 0),
            'call_max_session_send_after_recv_us' => (int) ($result['call_max_session_send_after_recv_us'][$i] ?? 0),
            'call_poll_wait_us' => (int) ($result['call_poll_wait_us'][$i] ?? 0),
            'call_max_poll_wait_us' => (int) ($result['call_max_poll_wait_us'][$i] ?? 0),
            'call_pollin_ready' => (int) ($result['call_pollin_ready'][$i] ?? 0),
            'call_pollout_ready' => (int) ($result['call_pollout_ready'][$i] ?? 0),
            'call_poll_to_data_us' => (int) ($result['call_poll_to_data_us'][$i] ?? 0),
            'call_max_poll_to_data_us' => (int) ($result['call_max_poll_to_data_us'][$i] ?? 0),
            'call_window_update_to_data_us' => (int) ($result['call_window_update_to_data_us'][$i] ?? 0),
            'call_max_window_update_to_data_us' => (int) ($result['call_max_window_update_to_data_us'][$i] ?? 0),
            'call_receive_drains' => (int) ($result['call_receive_drains'][$i] ?? 0),
            'call_receive_drains_with_data' => (int) ($result['call_receive_drains_with_data'][$i] ?? 0),
            'call_receive_drains_eagain_after_data' => (int) ($result['call_receive_drains_eagain_after_data'][$i] ?? 0),
            'call_max_reads_per_drain' => (int) ($result['call_max_reads_per_drain'][$i] ?? 0),
            'call_max_bytes_per_drain' => (int) ($result['call_max_bytes_per_drain'][$i] ?? 0),
            'call_min_session_remote_window' => (int) ($result['call_min_session_remote_window'][$i] ?? 0),
            'call_min_stream_remote_window' => (int) ($result['call_min_stream_remote_window'][$i] ?? 0),
            'call_response_data_bytes' => (int) ($result['call_response_data_bytes'][$i] ?? 0),
            'call_data_recv_calls' => (int) ($result['call_data_recv_calls'][$i] ?? 0),
            'call_body_append_us' => (int) ($result['call_body_append_us'][$i] ?? 0),
            'call_max_body_append_us' => (int) ($result['call_max_body_append_us'][$i] ?? 0),
            'call_body_compact_count' => (int) ($result['call_body_compact_count'][$i] ?? 0),
            'call_body_compact_bytes' => (int) ($result['call_body_compact_bytes'][$i] ?? 0),
            'call_body_compact_us' => (int) ($result['call_body_compact_us'][$i] ?? 0),
            'call_max_body_compact_us' => (int) ($result['call_max_body_compact_us'][$i] ?? 0),
            'call_max_body_buffer_bytes' => (int) ($result['call_max_body_buffer_bytes'][$i] ?? 0),
            'call_decoded_messages' => (int) ($result['call_decoded_messages'][$i] ?? 0),
            'call_max_response_queue_count' => (int) ($result['call_max_response_queue_count'][$i] ?? 0),
            'call_max_response_queue_bytes' => (int) ($result['call_max_response_queue_bytes'][$i] ?? 0),
            'call_response_queue_wait_us' => (int) ($result['call_response_queue_wait_us'][$i] ?? 0),
            'call_max_response_queue_wait_us' => (int) ($result['call_max_response_queue_wait_us'][$i] ?? 0),
            'call_response_payload_string_us' => (int) ($result['call_response_payload_string_us'][$i] ?? 0),
            'call_max_response_payload_string_us' => (int) ($result['call_max_response_payload_string_us'][$i] ?? 0),
            'call_response_decode_us' => (int) ($result['call_response_decode_us'][$i] ?? 0),
            'call_max_response_decode_us' => (int) ($result['call_max_response_decode_us'][$i] ?? 0),
            'server_handler_ns' => (int) ($result['server_handler_ns'][$i] ?? 0),
            'server_stats_handler_start_ns' => (int) ($result['server_stats_handler_start_ns'][$i] ?? 0),
            'server_stats_handler_end_ns' => (int) ($result['server_stats_handler_end_ns'][$i] ?? 0),
            'server_stats_in_payload_ns' => (int) ($result['server_stats_in_payload_ns'][$i] ?? 0),
            'server_stats_out_header_ns' => (int) ($result['server_stats_out_header_ns'][$i] ?? 0),
            'server_stats_out_payload_ns' => (int) ($result['server_stats_out_payload_ns'][$i] ?? 0),
            'server_stats_first_out_payload_ns' => (int) ($result['server_stats_first_out_payload_ns'][$i] ?? 0),
            'server_stats_last_out_payload_ns' => (int) ($result['server_stats_last_out_payload_ns'][$i] ?? 0),
            'server_stats_out_payload_count' => (int) ($result['server_stats_out_payload_count'][$i] ?? 0),
        ];
    }
    return $samples;
}

/**
 * @param array<string, mixed> $result
 * @return list<array<string, int|float>>
 */
function tailSampleCalls(array $result, array $latencies, int $limit, array $derivedSeries = []): array
{
    $indices = array_keys($latencies);
    usort($indices, static fn (int $a, int $b): int => ($latencies[$b] ?? 0) <=> ($latencies[$a] ?? 0));

    $samples = [];
    foreach (array_slice($indices, 0, $limit) as $index) {
        $samples[] = [
            'index' => $index,
            'latency_us' => (int) ($latencies[$index] ?? 0),
            'client_upload_complete_us' => (int) ($result['client_upload_complete_us'][$index] ?? 0),
            'client_first_response_data_us' => (int) ($result['client_first_response_data_us'][$index] ?? 0),
            'client_last_response_data_us' => (int) ($result['client_last_response_data_us'][$index] ?? 0),
            'client_first_window_update_us' => (int) ($result['client_first_window_update_us'][$index] ?? 0),
            'client_last_window_update_us' => (int) ($result['client_last_window_update_us'][$index] ?? 0),
            'client_first_window_update_sent_us' => (int) ($result['client_first_window_update_sent_us'][$index] ?? 0),
            'client_last_window_update_sent_us' => (int) ($result['client_last_window_update_sent_us'][$index] ?? 0),
            'client_first_flow_control_pause_us' => (int) ($result['client_first_flow_control_pause_us'][$index] ?? 0),
            'client_response_header_us' => (int) ($result['client_response_header_us'][$index] ?? 0),
            'client_stream_close_us' => (int) ($result['client_stream_close_us'][$index] ?? 0),
            'client_last_response_message_ready_us' => (int) ($result['client_last_response_message_ready_us'][$index] ?? 0),
            'client_last_response_callback_done_us' => (int) ($result['client_last_response_callback_done_us'][$index] ?? 0),
            'call_loop_active_us' => (int) ($derivedSeries['call_loop_active_us'][$index] ?? 0),
            'call_loop_known_including_poll_us' => (int) ($derivedSeries['call_loop_known_including_poll_us'][$index] ?? 0),
            'call_unaccounted_after_loop_known_us' => (int) ($derivedSeries['call_unaccounted_after_loop_known_us'][$index] ?? 0),
            'call_mem_recv_inner_known_us' => (int) ($derivedSeries['call_mem_recv_inner_known_us'][$index] ?? 0),
            'call_after_last_data_to_close_us' => (int) ($derivedSeries['call_after_last_data_to_close_us'][$index] ?? 0),
            'call_after_message_ready_to_close_us' => (int) ($derivedSeries['call_after_message_ready_to_close_us'][$index] ?? 0),
            'call_after_callback_done_to_close_us' => (int) ($derivedSeries['call_after_callback_done_to_close_us'][$index] ?? 0),
            'call_server_last_to_close_us' => (int) ($derivedSeries['call_server_last_to_close_us'][$index] ?? 0),
            'call_window_update_frames_recv' => (int) ($result['call_window_update_frames_recv'][$index] ?? 0),
            'call_connection_window_update_frames_recv' => (int) ($result['call_connection_window_update_frames_recv'][$index] ?? 0),
            'call_stream_window_update_frames_recv' => (int) ($result['call_stream_window_update_frames_recv'][$index] ?? 0),
            'call_connection_window_update_increment_recv' => (int) ($result['call_connection_window_update_increment_recv'][$index] ?? 0),
            'call_stream_window_update_increment_recv' => (int) ($result['call_stream_window_update_increment_recv'][$index] ?? 0),
            'call_window_update_frames_sent' => (int) ($result['call_window_update_frames_sent'][$index] ?? 0),
            'call_connection_window_update_frames_sent' => (int) ($result['call_connection_window_update_frames_sent'][$index] ?? 0),
            'call_stream_window_update_frames_sent' => (int) ($result['call_stream_window_update_frames_sent'][$index] ?? 0),
            'call_connection_window_update_increment_sent' => (int) ($result['call_connection_window_update_increment_sent'][$index] ?? 0),
            'call_stream_window_update_increment_sent' => (int) ($result['call_stream_window_update_increment_sent'][$index] ?? 0),
            'call_data_read_length_calls' => (int) ($result['call_data_read_length_calls'][$index] ?? 0),
            'call_flow_control_pauses' => (int) ($result['call_flow_control_pauses'][$index] ?? 0),
            'call_max_write_syscall_us' => (int) ($result['call_max_write_syscall_us'][$index] ?? 0),
            'call_recv_syscalls' => (int) ($result['call_recv_syscalls'][$index] ?? 0),
            'call_recv_syscall_us' => (int) ($result['call_recv_syscall_us'][$index] ?? 0),
            'call_max_recv_syscall_us' => (int) ($result['call_max_recv_syscall_us'][$index] ?? 0),
            'call_mem_recv_us' => (int) ($result['call_mem_recv_us'][$index] ?? 0),
            'call_max_mem_recv_us' => (int) ($result['call_max_mem_recv_us'][$index] ?? 0),
            'call_session_send_after_recv_us' => (int) ($result['call_session_send_after_recv_us'][$index] ?? 0),
            'call_max_session_send_after_recv_us' => (int) ($result['call_max_session_send_after_recv_us'][$index] ?? 0),
            'call_poll_wait_us' => (int) ($result['call_poll_wait_us'][$index] ?? 0),
            'call_max_poll_wait_us' => (int) ($result['call_max_poll_wait_us'][$index] ?? 0),
            'call_pollin_ready' => (int) ($result['call_pollin_ready'][$index] ?? 0),
            'call_pollout_ready' => (int) ($result['call_pollout_ready'][$index] ?? 0),
            'call_poll_to_data_us' => (int) ($result['call_poll_to_data_us'][$index] ?? 0),
            'call_max_poll_to_data_us' => (int) ($result['call_max_poll_to_data_us'][$index] ?? 0),
            'call_window_update_to_data_us' => (int) ($result['call_window_update_to_data_us'][$index] ?? 0),
            'call_max_window_update_to_data_us' => (int) ($result['call_max_window_update_to_data_us'][$index] ?? 0),
            'call_receive_drains' => (int) ($result['call_receive_drains'][$index] ?? 0),
            'call_receive_drains_with_data' => (int) ($result['call_receive_drains_with_data'][$index] ?? 0),
            'call_receive_drains_eagain_after_data' => (int) ($result['call_receive_drains_eagain_after_data'][$index] ?? 0),
            'call_max_reads_per_drain' => (int) ($result['call_max_reads_per_drain'][$index] ?? 0),
            'call_max_bytes_per_drain' => (int) ($result['call_max_bytes_per_drain'][$index] ?? 0),
            'call_min_session_remote_window' => (int) ($result['call_min_session_remote_window'][$index] ?? 0),
            'call_min_stream_remote_window' => (int) ($result['call_min_stream_remote_window'][$index] ?? 0),
            'call_response_data_bytes' => (int) ($result['call_response_data_bytes'][$index] ?? 0),
            'call_data_recv_calls' => (int) ($result['call_data_recv_calls'][$index] ?? 0),
            'call_body_append_us' => (int) ($result['call_body_append_us'][$index] ?? 0),
            'call_max_body_append_us' => (int) ($result['call_max_body_append_us'][$index] ?? 0),
            'call_body_compact_count' => (int) ($result['call_body_compact_count'][$index] ?? 0),
            'call_body_compact_bytes' => (int) ($result['call_body_compact_bytes'][$index] ?? 0),
            'call_body_compact_us' => (int) ($result['call_body_compact_us'][$index] ?? 0),
            'call_max_body_compact_us' => (int) ($result['call_max_body_compact_us'][$index] ?? 0),
            'call_max_body_buffer_bytes' => (int) ($result['call_max_body_buffer_bytes'][$index] ?? 0),
            'call_decoded_messages' => (int) ($result['call_decoded_messages'][$index] ?? 0),
            'call_max_response_queue_count' => (int) ($result['call_max_response_queue_count'][$index] ?? 0),
            'call_max_response_queue_bytes' => (int) ($result['call_max_response_queue_bytes'][$index] ?? 0),
            'call_response_queue_wait_us' => (int) ($result['call_response_queue_wait_us'][$index] ?? 0),
            'call_max_response_queue_wait_us' => (int) ($result['call_max_response_queue_wait_us'][$index] ?? 0),
            'call_response_payload_string_us' => (int) ($result['call_response_payload_string_us'][$index] ?? 0),
            'call_max_response_payload_string_us' => (int) ($result['call_max_response_payload_string_us'][$index] ?? 0),
            'call_response_decode_us' => (int) ($result['call_response_decode_us'][$index] ?? 0),
            'call_max_response_decode_us' => (int) ($result['call_max_response_decode_us'][$index] ?? 0),
            'server_handler_ns' => (int) ($result['server_handler_ns'][$index] ?? 0),
            'server_stats_handler_start_ns' => (int) ($result['server_stats_handler_start_ns'][$index] ?? 0),
            'server_stats_handler_end_ns' => (int) ($result['server_stats_handler_end_ns'][$index] ?? 0),
            'server_stats_in_payload_ns' => (int) ($result['server_stats_in_payload_ns'][$index] ?? 0),
            'server_stats_out_header_ns' => (int) ($result['server_stats_out_header_ns'][$index] ?? 0),
            'server_stats_out_payload_ns' => (int) ($result['server_stats_out_payload_ns'][$index] ?? 0),
            'server_stats_first_out_payload_ns' => (int) ($result['server_stats_first_out_payload_ns'][$index] ?? 0),
            'server_stats_last_out_payload_ns' => (int) ($result['server_stats_last_out_payload_ns'][$index] ?? 0),
            'server_stats_out_payload_count' => (int) ($result['server_stats_out_payload_count'][$index] ?? 0),
        ];
    }
    return $samples;
}
