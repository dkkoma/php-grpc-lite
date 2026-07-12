--TEST--
grpc fatal nghttp2 RST_STREAM submit (fault injection) marks the connection dead and terminal for reuse
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-038.jsonl
GRPC_LITE_TEST_FAULT=rst-submit-fatal
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

// Every nghttp2_submit_rst_stream call in this process simulates a fatal
// NGHTTP2_ERR_NOMEM (GRPC_LITE_TEST_FAULT=rst-submit-fatal). After a fatal
// return the nghttp2 error contract allows only nghttp2_session_del(), so
// the connection must go dead, no RST may reach the wire, and every later
// call must run on a fresh connection.
$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

// 1. deadline expiry: the cancel path hits the fatal submit. The status
// stays DEADLINE_EXCEEDED (timed_out outranks the connection failure).
$request = new BenchRequest();
$request->setServerDelayMs(2000);
[, $status] = $client->BenchUnary($request, [], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, 'unary deadline status');

// 2. the dead connection must not be reused.
$hello = new HelloRequest();
$hello->setName('AfterFatal');
[, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'follow-up unary status');

// 3. response-policy RST inside a session callback (message too large): the
// policy flag decides the status, the fatal submit kills the connection.
$clientLimited = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
    'grpc.max_receive_message_length' => 8,
]);
$request = new BenchRequest();
$request->setMessageCount(2);
$request->setPayloadBytes(1024);
$call = $clientLimited->BenchServerStream($request, []);
foreach ($call->responses() as $_reply) {
}
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code, 'policy status survives fatal submit');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$prefaceCount = 0;
$followUpEnd = null;
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        throw new RuntimeException('no RST_STREAM may reach the wire after a fatal submit: ' . json_encode($record));
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello') {
        $followUpEnd = $record;
    }
}

grpc_lite_phpt_assert_same(false, $followUpEnd['persistent_reused'] ?? null, 'dead connection was not reused');
// Call 1 opens connection #1 and kills it (fatal cancel submit). Calls 2 and
// 3 share connection #2 (same connection key; message-length limits are not
// part of the key), which call 3's fatal policy submit kills again.
grpc_lite_phpt_assert_same(2, $prefaceCount, 'every call after a fatal ran on a fresh connection');

echo "OK\n";
?>
--EXPECT--
OK
