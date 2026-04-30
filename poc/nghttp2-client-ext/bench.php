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
]);

$iterations = (int) ($options['iterations'] ?? 1000);
$responseBytes = (int) ($options['response-bytes'] ?? 100);
$requestBytes = (int) ($options['request-bytes'] ?? 0);
$splitGrpcFrame = array_key_exists('split-grpc-frame', $options);
$noCopy = array_key_exists('no-copy', $options);
$dataFrameSize = (int) ($options['data-frame-size'] ?? 0);

$request = new Helloworld\BenchRequest();
$request->setPayloadBytes($responseBytes);
if ($requestBytes > 0) {
    $request->setRequestPayload(str_repeat("\0", $requestBytes));
}

$payload = $request->serializeToString();
$requestBody = $splitGrpcFrame ? $payload : "\0" . pack('N', strlen($payload)) . $payload;

$result = nghttp2_poc_unary_batch('test-server', 50051, '/helloworld.Greeter/BenchUnary', $requestBody, $iterations, [
    'x-bench-server-cached-payload' => '1',
], $splitGrpcFrame, $noCopy, $dataFrameSize);

$latencies = $result['latencies_us'];
sort($latencies);
$count = count($latencies);
$p50 = percentile($latencies, 0.50);
$p99 = percentile($latencies, 0.99);
$callsPerSecond = $result['total_us'] > 0 ? ($result['ok'] / ($result['total_us'] / 1_000_000)) : 0.0;

unset($result['latencies_us']);
$result += [
    'response_bytes' => $responseBytes,
    'request_bytes' => $requestBytes,
    'serialized_payload_bytes' => strlen($payload),
    'split_grpc_frame' => $splitGrpcFrame,
    'no_copy' => $noCopy,
    'data_frame_size' => $dataFrameSize,
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
