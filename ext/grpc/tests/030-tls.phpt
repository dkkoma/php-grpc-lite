--TEST--
grpc TLS and mTLS credential paths work against local test server
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50052, 50053]);
foreach (['server.crt', 'client.crt', 'client.key'] as $certFile) {
    if (!is_file(grpc_lite_phpt_repo_root() . '/poc/test-server/certs/' . $certFile)) {
        die('skip test certificates are not available');
    }
}
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$certDir = grpc_lite_phpt_repo_root() . '/poc/test-server/certs';
$root = file_get_contents($certDir . '/server.crt');
$clientCert = file_get_contents($certDir . '/client.crt');
$clientKey = file_get_contents($certDir . '/client.key');
grpc_lite_phpt_assert_true($root !== false && $clientCert !== false && $clientKey !== false, 'cert fixtures');

$request = new HelloRequest();
$request->setName('TLS');
$tlsClient = new GreeterClient('test-server:50052', [
    'credentials' => ChannelCredentials::createSsl($root),
]);
[$response, $status] = $tlsClient->SayHello($request)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'TLS status');
grpc_lite_phpt_assert_same('Hello, TLS', $response->getMessage(), 'TLS response');

$request = new HelloRequest();
$request->setName('mTLS');
$mtlsClient = new GreeterClient('test-server:50053', [
    'credentials' => ChannelCredentials::createSsl($root, $clientKey, $clientCert),
]);
[$response, $status] = $mtlsClient->SayHello($request)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'mTLS status');
grpc_lite_phpt_assert_same('Hello, mTLS', $response->getMessage(), 'mTLS response');

$badClient = new GreeterClient('test-server:50052', [
    'credentials' => ChannelCredentials::createSsl("-----BEGIN CERTIFICATE-----\ninvalid\n-----END CERTIFICATE-----\n"),
]);
[, $badStatus] = $badClient->SayHello(new HelloRequest())->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $badStatus->code, 'bad TLS root status');

echo "OK\n";
?>
--EXPECT--
OK
