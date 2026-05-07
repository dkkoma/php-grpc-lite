--TEST--
grpc response message and metadata limits return RESOURCE_EXHAUSTED
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

$limitedClient = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'grpc.max_receive_message_length' => 10,
]);

$request = new BenchRequest();
$request->setPayloadBytes(100);
[$response, $status] = $limitedClient->BenchUnary($request)->wait();
grpc_lite_phpt_assert_same(null, $response, 'unary too large response');
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $status->code, 'unary too large status');

$request = new BenchRequest();
$request->setMessageCount(1);
$request->setPayloadBytes(100);
$call = $limitedClient->BenchServerStream($request);
$count = 0;
foreach ($call->responses() as $_reply) {
    $count++;
}
grpc_lite_phpt_assert_same(0, $count, 'stream too large count');
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code, 'stream too large status');

$metadataLimitedClient = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'grpc.absolute_max_metadata_size' => 1024,
]);
$metadata = [
    'x-bench-response-metadata-count' => ['8'],
    'x-bench-response-metadata-value-bytes' => ['512'],
];
[, $status] = $metadataLimitedClient->BenchUnary(new BenchRequest(), $metadata)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $status->code, 'unary metadata too large status');

$request = new BenchRequest();
$request->setMessageCount(1);
$call = $metadataLimitedClient->BenchServerStream($request, $metadata);
$count = 0;
foreach ($call->responses() as $_reply) {
    $count++;
}
grpc_lite_phpt_assert_same(0, $count, 'stream metadata too large count');
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code, 'stream metadata too large status');

echo "OK\n";
?>
--EXPECT--
OK
