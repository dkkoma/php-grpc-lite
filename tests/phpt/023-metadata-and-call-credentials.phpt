--TEST--
grpc binary metadata, duplicate metadata, and call credentials round-trip
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051, 50052]);
if (!is_file(grpc_lite_phpt_repo_root() . '/poc/test-server/certs/server.crt')) {
    die('skip test certificate is not available');
}
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$certDir = grpc_lite_phpt_repo_root() . '/poc/test-server/certs';
$root = file_get_contents($certDir . '/server.crt');
grpc_lite_phpt_assert_true($root !== false, 'cert fixture');

$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);
$tlsClient = new GreeterClient('test-server:50052', [
    'credentials' => ChannelCredentials::createSsl($root),
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
        return ['x-bench-echo-ascii' => 'from-plugin'];
    },
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_true(!$callbackCalled, 'insecure call credentials callback must not be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAUTHENTICATED, $status->code, 'insecure call credentials status');
grpc_lite_phpt_assert_contains('secure channel', $status->details, 'insecure call credentials details');

$tlsCallbackCalled = false;
$tlsCall = $tlsClient->BenchUnary(new BenchRequest(), [], [
    'call_credentials_callback' => static function (string $serviceUrl, string $methodName) use (&$tlsCallbackCalled): array {
        $tlsCallbackCalled = true;
        grpc_lite_phpt_assert_contains('https://test-server:50052/helloworld.Greeter', $serviceUrl, 'TLS call credentials service URL');
        grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $methodName, 'TLS call credentials method');
        return ['x-bench-echo-ascii' => 'from-plugin'];
    },
]);
[, $tlsStatus] = $tlsCall->wait();
grpc_lite_phpt_assert_true($tlsCallbackCalled, 'TLS call credentials callback must be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $tlsStatus->code, 'TLS call credentials status');
grpc_lite_phpt_assert_same(['from-plugin'], $tlsCall->getMetadata()['x-bench-initial-ascii'] ?? null, 'TLS call credentials metadata');

$apiClientMetricsCall = $tlsClient->BenchUnary(new BenchRequest(), [
    'x-bench-observe-metadata-key' => ['x-goog-api-client'],
    'x-goog-api-client' => ['gl-php/test gax/test grpc/test'],
], [
    'call_credentials_callback' => static fn (): array => ['x-goog-api-client' => ['cred-type/u']],
]);
[, $apiClientMetricsStatus] = $apiClientMetricsCall->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $apiClientMetricsStatus->code, 'x-goog-api-client call credentials status');
$apiClientMetricsMetadata = $apiClientMetricsCall->getMetadata();
grpc_lite_phpt_assert_same('x-goog-api-client', $apiClientMetricsMetadata['x-bench-seen-000-key-bin'][0] ?? null, 'x-goog-api-client observed key');
grpc_lite_phpt_assert_same('1', $apiClientMetricsMetadata['x-bench-seen-000-count'][0] ?? null, 'x-goog-api-client must be folded to one metadata value');
grpc_lite_phpt_assert_same('gl-php/test gax/test grpc/test cred-type/u', $apiClientMetricsMetadata['x-bench-seen-000-value-000-bin'][0] ?? null, 'x-goog-api-client folded value');

$emptyApiClientMetricsCall = $tlsClient->BenchUnary(new BenchRequest(), [
    'x-bench-observe-metadata-key' => ['x-goog-api-client'],
], [
    'call_credentials_callback' => static fn (): array => ['x-goog-api-client' => []],
]);
[, $emptyApiClientMetricsStatus] = $emptyApiClientMetricsCall->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $emptyApiClientMetricsStatus->code, 'empty x-goog-api-client call credentials status');
$emptyApiClientMetricsMetadata = $emptyApiClientMetricsCall->getMetadata();
grpc_lite_phpt_assert_same('x-goog-api-client', $emptyApiClientMetricsMetadata['x-bench-seen-000-key-bin'][0] ?? null, 'empty x-goog-api-client observed key');
grpc_lite_phpt_assert_same('0', $emptyApiClientMetricsMetadata['x-bench-seen-000-count'][0] ?? null, 'empty x-goog-api-client must not emit an empty metadata value');
grpc_lite_phpt_assert_same(null, $emptyApiClientMetricsMetadata['x-bench-seen-000-value-000-bin'][0] ?? null, 'empty x-goog-api-client emitted value');

$duplicateCredentialsCallbackCalled = false;
$duplicateCredentialsCall = $tlsClient->BenchUnary(new BenchRequest(), [
    'x-bench-echo-ascii' => ['from-request'],
], [
    'call_credentials_callback' => static function () use (&$duplicateCredentialsCallbackCalled): array {
        $duplicateCredentialsCallbackCalled = true;
        return ['x-bench-echo-ascii' => ['from-plugin']];
    },
]);
[, $duplicateCredentialsStatus] = $duplicateCredentialsCall->wait();
grpc_lite_phpt_assert_true($duplicateCredentialsCallbackCalled, 'duplicate call credentials callback must be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $duplicateCredentialsStatus->code, 'duplicate call credentials status');
grpc_lite_phpt_assert_same(['from-request', 'from-plugin'], $duplicateCredentialsCall->getMetadata()['x-bench-initial-ascii'] ?? null, 'duplicate call credentials metadata must append values');

grpc_lite_phpt_expect_throw(static fn () => $tlsClient->BenchUnary(new BenchRequest(), [], [
    'call_credentials_callback' => static fn (): string => 'not metadata',
])->wait(), 'must return an array');

$streamCallbackCalled = false;
$streamRequest = (new BenchRequest())->setMessageCount(1);
$streamCall = $client->BenchServerStream($streamRequest, [], [
    'call_credentials_callback' => static function () use (&$streamCallbackCalled): array {
        $streamCallbackCalled = true;
        return ['x-bench-echo-ascii' => 'from-plugin-stream'];
    },
]);
grpc_lite_phpt_assert_same([], iterator_to_array($streamCall->responses()), 'insecure stream call credentials responses');
$streamStatus = $streamCall->getStatus();
grpc_lite_phpt_assert_true(!$streamCallbackCalled, 'insecure stream call credentials callback must not be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAUTHENTICATED, $streamStatus->code, 'insecure stream call credentials status');

echo "OK\n";
?>
--EXPECT--
OK
