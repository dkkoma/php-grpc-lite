--TEST--
grpc status, content-type, malformed frame, and compression errors are mapped
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50054]);
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$client = new GreeterClient('test-server:50054', [
    'credentials' => ChannelCredentials::createInsecure(),
]);

$assertUnaryStatus = static function (array $metadata, int $code, string $details) use ($client): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), $metadata)->wait();
    grpc_lite_phpt_assert_same(null, $response, 'unary error response');
    grpc_lite_phpt_assert_same($code, $status->code, 'unary error status');
    grpc_lite_phpt_assert_same($details, $status->details, 'unary error details');
};

$assertUnaryStatus(
    ['x-bench-http-status' => ['503']],
    Grpc\STATUS_UNAVAILABLE,
    'HTTP status 503 without grpc-status',
);
$assertUnaryStatus(
    ['x-bench-content-type' => ['text/plain'], 'x-bench-grpc-status' => ['0']],
    Grpc\STATUS_UNKNOWN,
    'invalid gRPC content-type: text/plain',
);
$assertUnaryStatus(
    ['x-bench-grpc-status' => ['abc']],
    Grpc\STATUS_UNKNOWN,
    'invalid grpc-status trailer',
);
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['compressed-flag']],
    Grpc\STATUS_UNIMPLEMENTED,
    'compressed gRPC messages are not supported',
);
$assertUnaryStatus(
    ['x-bench-grpc-encoding' => ['gzip']],
    Grpc\STATUS_UNIMPLEMENTED,
    'unsupported grpc-encoding: gzip',
);
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['partial-frame']],
    Grpc\STATUS_INTERNAL,
    'malformed gRPC response frame',
);
// message 途中の RST_STREAM は malformed ではなく stream reset の taxonomy に従う
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['partial-frame-abort']],
    Grpc\STATUS_INTERNAL,
    'HTTP/2 stream reset: 2',
);
// unary は Length-Prefixed-Message ちょうど 1 個: 2 個目は malformed として弾く
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['two-messages']],
    Grpc\STATUS_INTERNAL,
    'malformed gRPC response frame',
);

// 空 message + grpc-status 0 は「空 payload の message + OK」として届く
[$emptyResponse, $emptyStatus] = $client->BenchUnary(new BenchRequest(), ['x-bench-grpc-status' => ['0']])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $emptyStatus->code, 'empty message unary status');
grpc_lite_phpt_assert_true($emptyResponse instanceof \Helloworld\BenchReply, 'empty message unary response type');

$assertStreamStatus = static function (array $metadata, int $code, string $details, int $expectedCount = 0) use ($client): void {
    $request = new BenchRequest();
    $request->setMessageCount(10);
    $call = $client->BenchServerStream($request, $metadata);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same($expectedCount, $count, 'stream error yield count');
    grpc_lite_phpt_assert_same($code, $call->getStatus()->code, 'stream error status');
    grpc_lite_phpt_assert_same($details, $call->getStatus()->details, 'stream error details');
};

$request = new BenchRequest();
$request->setMessageCount(10);
$call = $client->BenchServerStream($request, [
    'x-bench-content-type' => ['application/grpcfoo'],
]);
$count = 0;
foreach ($call->responses() as $_reply) {
    $count++;
}
grpc_lite_phpt_assert_same(0, $count, 'invalid stream content-type yield count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $call->getStatus()->code, 'invalid stream content-type status');
grpc_lite_phpt_assert_same('invalid gRPC content-type: application/grpcfoo', $call->getStatus()->details, 'invalid stream content-type details');

$assertStreamStatus(
    ['x-bench-grpc-response' => ['compressed-flag']],
    Grpc\STATUS_UNIMPLEMENTED,
    'compressed gRPC messages are not supported',
);
$assertStreamStatus(
    ['x-bench-grpc-encoding' => ['gzip']],
    Grpc\STATUS_UNIMPLEMENTED,
    'unsupported grpc-encoding: gzip',
);
$assertStreamStatus(
    ['x-bench-grpc-status' => ['abc']],
    Grpc\STATUS_UNKNOWN,
    'invalid grpc-status trailer',
    1,
);
$assertStreamStatus(
    ['x-bench-http-status' => ['503'], 'x-bench-content-type' => ['application/grpc']],
    Grpc\STATUS_UNAVAILABLE,
    'HTTP status 503 without grpc-status',
);

$request = new BenchRequest();
$request->setMessageCount(10);
$call = $client->BenchServerStream($request, [
    'x-bench-grpc-response' => ['partial-frame'],
]);
$count = 0;
foreach ($call->responses() as $_reply) {
    $count++;
}
grpc_lite_phpt_assert_same(0, $count, 'partial frame yield count');
grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $call->getStatus()->code, 'partial frame status');
grpc_lite_phpt_assert_contains('malformed gRPC response frame', $call->getStatus()->details, 'partial frame details');

echo "OK\n";
?>
--EXPECT--
OK
