--TEST--
grpc -bin request metadata is sent as un-padded base64
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-032.jsonl
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

// trace env はプロセス診断として MINIT で 1 回だけ読まれるため、
// 実行時 putenv() ではなく --ENV-- セクションで PHP 起動前に設定する。
$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

// 1/2/3 バイト境界: base64 パディングが 2 個 / 1 個 / 0 個になる値。
$binaryValues = ["\x00", "\x00\x01", "\x00\x01\xff"];

$call = $client->BenchUnary(new BenchRequest(), [
    'x-bench-observe-metadata-key' => ['x-bench-pad-bin'],
    'x-bench-pad-bin' => $binaryValues,
]);
[, $status] = $call->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status');

// サーバー側デコード結果の round-trip 確認(パディング除去後も受理されること)。
$responseMetadata = $call->getMetadata();
$count = (int) ($responseMetadata['x-bench-seen-000-count'][0] ?? 0);
$seen = [];
for ($index = 0; $index < $count; $index++) {
    $seen[] = $responseMetadata[sprintf('x-bench-seen-000-value-%03d-bin', $index)][0] ?? null;
}
grpc_lite_phpt_assert_same($binaryValues, $seen, 'server decodes un-padded -bin values');

// ワイヤ上の送信値をトレースから確認する。
$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$wireValues = [];
foreach ($lines as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.request_header'
        && ($record['name'] ?? null) === 'x-bench-pad-bin') {
        $wireValues[] = $record['value'] ?? null;
    }
}

$expectedWireValues = array_map(
    static fn (string $value): string => rtrim(base64_encode($value), '='),
    $binaryValues
);
grpc_lite_phpt_assert_same($expectedWireValues, $wireValues, '-bin wire values are un-padded base64');
foreach ($wireValues as $wireValue) {
    grpc_lite_phpt_assert_true(strpos((string) $wireValue, '=') === false, '-bin wire value must not contain padding');
}

echo "OK\n";
?>
--EXPECT--
OK
