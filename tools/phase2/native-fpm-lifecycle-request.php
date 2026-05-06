<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

header('content-type: application/json');

if (!extension_loaded('grpc')) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'grpc extension is not loaded'], JSON_THROW_ON_ERROR);
    return;
}

$request = "\0\0\0\0\0";
$result = grpc_lite_unary(
    'test-server:50051|50051',
    'test-server',
    50051,
    '/helloworld.Greeter/BenchUnary',
    $request,
    [],
    0,
    false,
    null,
    null,
    null,
    0,
    null,
    null,
);

echo json_encode([
    'ok' => $result['grpc_status'] === Grpc\STATUS_OK,
    'pid' => getmypid(),
    'grpc_status' => $result['grpc_status'],
    'persistent_reused' => (bool) ($result['persistent_reused'] ?? false),
    'connect_us' => (int) ($result['connect_us'] ?? -1),
    'channel_dead' => (bool) ($result['channel_dead'] ?? false),
    'details' => (string) ($result['grpc_message'] ?? ''),
], JSON_THROW_ON_ERROR);
