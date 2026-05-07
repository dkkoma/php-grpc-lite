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

$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);

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

$callbackCalled = false;
$call = $client->BenchUnary(new BenchRequest(), [], [
    'call_credentials_callback' => static function (string $serviceUrl, string $methodName) use (&$callbackCalled): array {
        $callbackCalled = true;
        grpc_lite_phpt_assert_contains('test-server:50051', $serviceUrl, 'call credentials service URL');
        grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $methodName, 'call credentials method');
        return ['x-bench-echo-ascii' => 'from-plugin'];
    },
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_true($callbackCalled, 'call credentials callback must be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'call credentials status');
grpc_lite_phpt_assert_same(['from-plugin'], $call->getMetadata()['x-bench-initial-ascii'] ?? null, 'call credentials metadata');

echo "OK\n";
?>
--EXPECT--
OK
