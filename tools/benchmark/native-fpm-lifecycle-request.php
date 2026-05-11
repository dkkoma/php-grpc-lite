<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

header('content-type: application/json');

if (!extension_loaded('grpc')) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'grpc extension is not loaded'], JSON_THROW_ON_ERROR);
    return;
}

$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);

$request = new BenchRequest();
[$response, $status] = $client->BenchUnary($request)->wait();

$ok = $status->code === Grpc\STATUS_OK && $response !== null;

echo json_encode([
    'ok' => $ok,
    'pid' => getmypid(),
    'grpc_status' => $status->code,
    'details' => (string) $status->details,
], JSON_THROW_ON_ERROR);
