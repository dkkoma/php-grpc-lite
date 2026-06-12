--TEST--
grpc unary request payload survives DATA frame boundaries (empty / 16KB-crossing / multi-frame / 1MB)
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

// 送信経路の境界ケース: 空 request (request_len == 0)、max frame size (16384)
// 前後で DATA フレーム分割が変わるサイズ、複数フレーム、1MB。
// x-bench-server-timing trailer の request-payload-bytes でサーバ受信長を固定する。
$sizes = [0, 16379, 16380, 16384, 65536, 1048576];

foreach ($sizes as $size) {
    $request = new BenchRequest();
    if ($size > 0) {
        $request->setRequestPayload(str_repeat('x', $size));
    }
    $call = $client->BenchUnary($request, ['x-bench-server-timing' => ['1']]);
    [$response, $status] = $call->wait();

    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, "upload {$size}B status");
    grpc_lite_phpt_assert_true($response !== null, "upload {$size}B response");
    $trailers = $call->getTrailingMetadata();
    grpc_lite_phpt_assert_same(
        (string) $size,
        $trailers['x-bench-server-request-payload-bytes'][0] ?? null,
        "upload {$size}B server-received length"
    );
}

echo "OK\n";
?>
--EXPECT--
OK
