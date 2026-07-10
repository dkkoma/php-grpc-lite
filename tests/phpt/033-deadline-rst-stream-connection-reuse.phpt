--TEST--
grpc deadline expiry sends RST_STREAM(CANCEL) and keeps the persistent connection reusable
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-033.jsonl
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

// 1. unary deadline expiry: the stream must be cancelled, not the connection.
$request = new BenchRequest();
$request->setServerDelayMs(500);
[, $status] = $client->BenchUnary($request, [], ['timeout' => 50_000])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, 'unary deadline status');

// 2. next unary call must reuse the same persistent connection.
$hello = new HelloRequest();
$hello->setName('AfterTimeout');
[$response, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status after timeout');
grpc_lite_phpt_assert_same('Hello, AfterTimeout', $response->getMessage(), 'unary reply after timeout');

// 3. server streaming deadline expiry keeps the connection reusable too.
$request = new BenchRequest();
$request->setMessageCount(5);
$request->setServerDelayMs(200);
$call = $client->BenchServerStream($request, [], ['timeout' => 50_000]);
foreach ($call->responses() as $_reply) {
}
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $call->getStatus()->code, 'streaming deadline status');

[$response, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status after streaming timeout');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$unaryEnds = [];
$rstFrames = [];
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_kind'] ?? null) === 'unary') {
        $unaryEnds[] = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $rstFrames[] = $record;
    }
}

// Wire evidence: both deadline expiries emitted RST_STREAM(CANCEL = 8).
grpc_lite_phpt_assert_same(2, count($rstFrames), 'outbound RST_STREAM count');
foreach ($rstFrames as $index => $frame) {
    grpc_lite_phpt_assert_same(8, $frame['error_code'] ?? null, "RST_STREAM #$index error code is CANCEL");
    grpc_lite_phpt_assert_true(($frame['stream_id'] ?? 0) > 0, "RST_STREAM #$index has a stream id");
}

// Connection evidence: every call after the first reused the persistent connection.
grpc_lite_phpt_assert_same(3, count($unaryEnds), 'unary rpc.end count');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $unaryEnds[0]['status_code'] ?? null, 'first unary rpc.end status');
grpc_lite_phpt_assert_same(false, $unaryEnds[0]['persistent_reused'] ?? null, 'first unary opened the connection');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $unaryEnds[1]['status_code'] ?? null, 'second unary rpc.end status');
grpc_lite_phpt_assert_same(true, $unaryEnds[1]['persistent_reused'] ?? null, 'connection reused after unary deadline expiry');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $unaryEnds[2]['status_code'] ?? null, 'third unary rpc.end status');
grpc_lite_phpt_assert_same(true, $unaryEnds[2]['persistent_reused'] ?? null, 'connection reused after streaming deadline expiry');

echo "OK\n";
?>
--EXPECT--
OK
