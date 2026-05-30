--TEST--
grpc deadline is enforced for unary and server streaming calls
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

$request = new BenchRequest();
$request->setServerDelayMs(100);
[, $status] = $client->BenchUnary($request, [], ['timeout' => 10_000])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, 'unary deadline status');

$request = new BenchRequest();
$request->setMessageCount(5);
$request->setServerDelayMs(50);
$call = $client->BenchServerStream($request, [], ['timeout' => 10_000]);
foreach ($call->responses() as $_reply) {
}
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $call->getStatus()->code, 'server streaming deadline status');

echo "OK\n";
?>
--EXPECT--
OK
