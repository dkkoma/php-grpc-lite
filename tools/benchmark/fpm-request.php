<?php
declare(strict_types=1);

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

require __DIR__ . '/../../vendor/autoload.php';

$case = $_SERVER['BENCH_CASE'] ?? 'small_unary_100b';
$target = $_SERVER['BENCH_TARGET'] ?? 'test-server:50051';

$client = new GreeterClient($target, [
    'credentials' => ChannelCredentials::createInsecure(),
]);

if ($case === 'small_unary_100b') {
    $request = new BenchRequest();
    $request->setPayloadBytes(100);
    [$response, $status] = $client->BenchUnary($request)->wait();
    if ($status->code !== Grpc\STATUS_OK || $response === null) {
        http_response_code(500);
        echo "BenchUnary failed: {$status->details}\n";
        return;
    }
    echo "OK\n";
    return;
}

if ($case === 'small_streaming_1x100b') {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $request->setPayloadBytes(100);
    $count = 0;
    foreach ($client->BenchServerStream($request)->responses() as $_reply) {
        $count++;
    }
    if ($count !== 1) {
        http_response_code(500);
        echo "expected 1 messages, got $count\n";
        return;
    }
    echo "OK\n";
    return;
}

http_response_code(400);
echo "unknown BENCH_CASE: $case\n";
