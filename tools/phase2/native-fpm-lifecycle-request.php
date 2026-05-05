<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

use Grpc\Internal\NativeTransport;

header('content-type: application/json');

if (!extension_loaded('grpc')) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'grpc extension is not loaded'], JSON_THROW_ON_ERROR);
    return;
}

$result = NativeTransport::unarySimple(
    'test-server:50051',
    '/helloworld.Greeter/BenchUnary',
    "\0\0\0\0\0",
    [],
);

$raw = $result['raw'];
echo json_encode([
    'ok' => $result['grpc_status'] === \Grpc\STATUS_OK,
    'pid' => getmypid(),
    'grpc_status' => $result['grpc_status'],
    'persistent_reused' => (bool) ($raw['persistent_reused'] ?? false),
    'connect_us' => (int) ($raw['connect_us'] ?? -1),
    'channel_dead' => (bool) ($raw['channel_dead'] ?? false),
    'details' => $result['details'],
], JSON_THROW_ON_ERROR);
