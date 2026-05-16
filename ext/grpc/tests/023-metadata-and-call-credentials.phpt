--TEST--
grpc binary metadata, duplicate metadata, and call credentials round-trip
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$clientOptions = [
    'credentials' => ChannelCredentials::createInsecure(),
];
$client = new GreeterClient('test-server:50051', $clientOptions);

$binaryValues = ["\x00\x01\xff,a"];
$call = $client->BenchUnary(new BenchRequest(), [
    'x-bench-echo-bin' => $binaryValues,
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'binary metadata status');
grpc_lite_phpt_assert_same($binaryValues, $call->getMetadata()['x-bench-initial-bin'] ?? null, 'binary initial metadata');
grpc_lite_phpt_assert_same($binaryValues, $call->getTrailingMetadata()['x-bench-trailing-bin'] ?? null, 'binary trailing metadata');

$asciiValues = ['one', 'two'];
$call = $client->BenchUnary(new BenchRequest(), [
    'x-bench-echo-ascii' => $asciiValues,
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'duplicate metadata status');
grpc_lite_phpt_assert_same($asciiValues, $call->getMetadata()['x-bench-initial-ascii'] ?? null, 'duplicate initial metadata');
grpc_lite_phpt_assert_same($asciiValues, $call->getTrailingMetadata()['x-bench-trailing-ascii'] ?? null, 'duplicate trailing metadata');

$callbackCalls = [];
$call = $client->BenchUnary(new BenchRequest(), [], [
    'call_credentials_callback' => static function (string $serviceUrl, string $methodName) use (&$callbackCalls): array {
        $callbackCalls[] = [$serviceUrl, $methodName];
        grpc_lite_phpt_assert_same('http://test-server:50051/helloworld.Greeter', $serviceUrl, 'call credentials unary service URL');
        grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $methodName, 'call credentials unary method');
        return ['x-bench-echo-ascii' => 'from-plugin'];
    },
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_same(1, count($callbackCalls), 'call credentials unary callback count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'call credentials status');
grpc_lite_phpt_assert_same(['from-plugin'], $call->getMetadata()['x-bench-initial-ascii'] ?? null, 'call credentials metadata');

$streamRequest = (new BenchRequest())->setMessageCount(1);
$streamCall = $client->BenchServerStream($streamRequest, [], [
    'call_credentials_callback' => static function (string $serviceUrl, string $methodName) use (&$callbackCalls): array {
        $callbackCalls[] = [$serviceUrl, $methodName];
        grpc_lite_phpt_assert_same('http://test-server:50051/helloworld.Greeter', $serviceUrl, 'call credentials streaming service URL');
        grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchServerStream', $methodName, 'call credentials streaming method');
        return ['x-bench-echo-ascii' => 'from-plugin-stream'];
    },
]);
iterator_to_array($streamCall->responses());
$streamStatus = $streamCall->getStatus();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $streamStatus->code, 'call credentials streaming status');
grpc_lite_phpt_assert_same(2, count($callbackCalls), 'call credentials total callback count');
grpc_lite_phpt_assert_same(['from-plugin-stream'], $streamCall->getMetadata()['x-bench-initial-ascii'] ?? null, 'call credentials streaming metadata');

$authorityClient = new GreeterClient('test-server:50051', $clientOptions + [
    'grpc.default_authority' => 'override.example.test',
]);
$authorityCall = $authorityClient->BenchUnary(new BenchRequest(), [], [
    'call_credentials_callback' => static function (string $serviceUrl, string $methodName): array {
        grpc_lite_phpt_assert_same('http://override.example.test/helloworld.Greeter', $serviceUrl, 'call credentials authority override service URL');
        grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $methodName, 'call credentials authority override method');
        return [];
    },
]);
[, $authorityStatus] = $authorityCall->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $authorityStatus->code, 'call credentials authority override status');

echo "OK\n";
?>
--EXPECT--
OK
