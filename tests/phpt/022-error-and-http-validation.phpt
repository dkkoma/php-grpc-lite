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
    Grpc\STATUS_INTERNAL,
    'compressed gRPC messages are not supported',
);
// grpc-encoding は宣言に過ぎない: flag=0 message は未対応 encoding 下でも成功する
[$gzipIdentityResponse, $gzipIdentityStatus] = $client->BenchUnary(new BenchRequest(), [
    'x-bench-grpc-encoding' => ['gzip'],
    'x-bench-grpc-status' => ['0'],
])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $gzipIdentityStatus->code, 'gzip advertised flag=0 unary status');
grpc_lite_phpt_assert_true($gzipIdentityResponse instanceof \Helloworld\BenchReply, 'gzip advertised flag=0 unary response type');
// gzip advertise + trailers-only non-OK は wire status をそのまま返す
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['headers-only'], 'x-bench-grpc-encoding' => ['gzip'], 'x-bench-grpc-status' => ['5']],
    Grpc\STATUS_NOT_FOUND,
    '',
);
// flag=1 かつ未対応 encoding は INTERNAL (encoding 起因の details)
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['compressed-flag'], 'x-bench-grpc-encoding' => ['gzip']],
    Grpc\STATUS_INTERNAL,
    'unsupported grpc-encoding: gzip',
);
// headers-only END_STREAM (grpc-status なし) は UNKNOWN のまま (grpc-go operateHeaders)
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['headers-only']],
    Grpc\STATUS_UNKNOWN,
    '',
);
// grpc-status を含まない trailing HEADERS も UNKNOWN
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['custom-trailers-no-status']],
    Grpc\STATUS_UNKNOWN,
    '',
);
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['grpc-message-only-trailers']],
    Grpc\STATUS_UNKNOWN,
    'trailers without status',
);
// message 受信後に trailers (grpc-status) なしで clean END_STREAM → INTERNAL
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['no-trailers']],
    Grpc\STATUS_INTERNAL,
    'server closed the stream without sending trailers',
);
// 1xx (103) 後の final response HEADERS は HCAT_HEADERS で届くが、
// non-terminal HEADERS なので DATA END_STREAM の trailers 欠落判定を抑止しない
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['no-trailers'], 'x-bench-early-hints' => ['1']],
    Grpc\STATUS_INTERNAL,
    'server closed the stream without sending trailers',
);
// 1xx 経由でも正常応答は成功する
[$earlyHintsResponse, $earlyHintsStatus] = $client->BenchUnary(new BenchRequest(), [
    'x-bench-grpc-status' => ['0'],
    'x-bench-early-hints' => ['1'],
])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $earlyHintsStatus->code, 'early hints unary status');
grpc_lite_phpt_assert_true($earlyHintsResponse instanceof \Helloworld\BenchReply, 'early hints unary response type');
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['partial-frame']],
    Grpc\STATUS_INTERNAL,
    'malformed gRPC response frame',
);
memory_reset_peak_usage();
$beforeLargeTruncated = memory_get_usage(true);
$assertUnaryStatus(
    ['x-bench-grpc-response' => ['declared-large-truncated']],
    Grpc\STATUS_INTERNAL,
    'malformed gRPC response frame',
);
$largeTruncatedPeakDelta = memory_get_peak_usage(true) - $beforeLargeTruncated;
grpc_lite_phpt_assert_true($largeTruncatedPeakDelta < 16 * 1024 * 1024, 'large declared truncated unary response must not allocate declared payload');
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
    Grpc\STATUS_INTERNAL,
    'compressed gRPC messages are not supported',
);
$assertStreamStatus(
    ['x-bench-grpc-encoding' => ['gzip'], 'x-bench-grpc-status' => ['0']],
    Grpc\STATUS_OK,
    '',
    1,
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['headers-only'], 'x-bench-grpc-encoding' => ['gzip'], 'x-bench-grpc-status' => ['5']],
    Grpc\STATUS_NOT_FOUND,
    '',
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['compressed-flag'], 'x-bench-grpc-encoding' => ['gzip']],
    Grpc\STATUS_INTERNAL,
    'unsupported grpc-encoding: gzip',
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['headers-only']],
    Grpc\STATUS_UNKNOWN,
    '',
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['custom-trailers-no-status']],
    Grpc\STATUS_UNKNOWN,
    '',
    1,
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['grpc-message-only-trailers']],
    Grpc\STATUS_UNKNOWN,
    'trailers without status',
    1,
);
$assertStreamStatus(
    ['x-bench-grpc-status' => ['abc']],
    Grpc\STATUS_UNKNOWN,
    'invalid grpc-status trailer',
    1,
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['no-trailers']],
    Grpc\STATUS_INTERNAL,
    'server closed the stream without sending trailers',
    1,
);
$assertStreamStatus(
    ['x-bench-grpc-response' => ['no-trailers'], 'x-bench-early-hints' => ['1']],
    Grpc\STATUS_INTERNAL,
    'server closed the stream without sending trailers',
    1,
);
$assertStreamStatus(
    ['x-bench-early-hints' => ['1'], 'x-bench-grpc-status' => ['0']],
    Grpc\STATUS_OK,
    '',
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

memory_reset_peak_usage();
$beforeLargeTruncatedStream = memory_get_usage(true);
$request = new BenchRequest();
$request->setMessageCount(10);
$call = $client->BenchServerStream($request, [
    'x-bench-grpc-response' => ['declared-large-truncated'],
]);
$count = 0;
foreach ($call->responses() as $_reply) {
    $count++;
}
grpc_lite_phpt_assert_same(0, $count, 'large declared truncated stream yield count');
grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $call->getStatus()->code, 'large declared truncated stream status');
grpc_lite_phpt_assert_contains('malformed gRPC response frame', $call->getStatus()->details, 'large declared truncated stream details');
$largeTruncatedStreamPeakDelta = memory_get_peak_usage(true) - $beforeLargeTruncatedStream;
grpc_lite_phpt_assert_true($largeTruncatedStreamPeakDelta < 16 * 1024 * 1024, 'large declared truncated stream response must not allocate declared payload');

echo "OK\n";
?>
--EXPECT--
OK
