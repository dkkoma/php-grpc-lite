--TEST--
grpc server streaming yields all messages and returns OK status
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
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);
$request = new HelloRequest();
$request->setName('Stream');

$call = $client->SayManyHellos($request);
$messages = [];
foreach ($call->responses() as $reply) {
    $messages[] = $reply->getMessage();
}

grpc_lite_phpt_assert_same([
    'Hello #1, Stream',
    'Hello #2, Stream',
    'Hello #3, Stream',
    'Hello #4, Stream',
    'Hello #5, Stream',
], $messages, 'server streaming messages');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $call->getStatus()->code, 'server streaming status');

$call = $client->SayManyHellos($request);
grpc_lite_phpt_assert_same('test-server:50051', $call->getPeer(), 'server streaming peer');
$call->cancel();
grpc_lite_phpt_assert_same(Grpc\STATUS_CANCELLED, $call->getStatus()->code, 'cancelled server streaming status');

echo "OK\n";
?>
--EXPECT--
OK
