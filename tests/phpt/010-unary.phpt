--TEST--
grpc unary call succeeds and exposes metadata/trailers
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

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\HelloReply;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

foreach (['Phase0', 'Reuse'] as $name) {
    $request = new HelloRequest();
    $request->setName($name);
    $call = $client->SayHello($request);
    [$response, $status] = $call->wait();

    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status');
    grpc_lite_phpt_assert_same('', $status->details, 'unary details');
    grpc_lite_phpt_assert_true($response instanceof HelloReply, 'unary response type');
    grpc_lite_phpt_assert_same("Hello, $name", $response->getMessage(), 'unary response message');

    $headers = $call->getMetadata();
    $trailers = $call->getTrailingMetadata();
    grpc_lite_phpt_assert_same('application/grpc', $headers['content-type'][0] ?? null, 'content-type metadata');
    // ext-grpc 互換: grpc-status / grpc-message は Status として消費され metadata には現れない
    grpc_lite_phpt_assert_true(!array_key_exists('grpc-status', $trailers), 'grpc-status not in trailers');
    grpc_lite_phpt_assert_true(!array_key_exists('grpc-message', $trailers), 'grpc-message not in trailers');
}

echo "OK\n";
?>
--EXPECT--
OK
