--TEST--
grpc fatal nghttp2 RST_STREAM submit (fault injection) marks the connection dead and terminal for reuse
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
grpc_lite_phpt_skip_if_test_fault_seam_unavailable();
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-038.jsonl
GRPC_LITE_TEST_FAULT=rst-submit-fatal,submit-request-fatal-decoy
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
// call must run on a fresh connection. The env also carries
// "submit-request-fatal-decoy": token matching must be exact, so the decoy
// must NOT trip the submit-request seam (a substring match would make every
// call in this test throw instead).
$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

// 1. deadline expiry: the cancel path hits the fatal submit. The deadline is
// the primary call outcome, so code AND details both stay deadline-specific
// (the fatal RST submit is secondary connection cleanup and must not leak
// "nghttp2 error: Out of memory" into the details).
$request = new BenchRequest();
$request->setServerDelayMs(2000);
[, $status] = $client->BenchUnary($request, [], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, 'unary deadline status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $status->details, 'unary deadline details');

// 2. the dead connection must not be reused.
$hello = new HelloRequest();
$hello->setName('AfterFatal');
[, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'follow-up unary status');

// The fault set is a MINIT-owned copy: later putenv() must neither change
// the active faults nor leave the seam reading freed environment storage
// (the original seam cached the raw getenv() pointer, which dangles after
// putenv()).
putenv('GRPC_LITE_TEST_FAULT');
putenv('GRPC_LITE_TEST_FAULT=submit-request-fatal');
[, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'putenv does not alter the MINIT fault snapshot');

// 3. server-streaming deadline between two pulls: the first message arrives,
// then the deadline expires before the next pull and the cancel path hits the
// fatal RST submit. Same contract as the unary case: exact deadline code AND
// details, and the killed connection is never reused.
$request = new BenchRequest();
$request->setMessageCount(5);
$request->setServerDelayMs(2000);
$deadlineCall = $client->BenchServerStream($request, [], ['timeout' => 300_000]);
$deadlineMessages = 0;
foreach ($deadlineCall->responses() as $_reply) {
    $deadlineMessages++;
}
grpc_lite_phpt_assert_same(1, $deadlineMessages, 'streaming deadline message count');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $deadlineCall->getStatus()->code, 'streaming deadline status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $deadlineCall->getStatus()->details, 'streaming deadline details');

// 4. response-policy RST inside a session callback (message too large): the
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
$sayHelloEnds = [];
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        throw new RuntimeException('no RST_STREAM may reach the wire after a fatal submit: ' . json_encode($record));
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello') {
        $sayHelloEnds[] = $record;
    }
}

grpc_lite_phpt_assert_same(2, count($sayHelloEnds), 'SayHello rpc.end count');
grpc_lite_phpt_assert_same(false, $sayHelloEnds[0]['persistent_reused'] ?? null, 'dead connection was not reused');
grpc_lite_phpt_assert_same(true, $sayHelloEnds[1]['persistent_reused'] ?? null, 'healthy connection stays reusable');
// Call 1 opens connection #1 and kills it (fatal cancel submit). The two
// SayHello calls and the deadline streaming call share connection #2, whose
// fatal cancel submit kills it again. The limited streaming call then opens
// connection #3 (same connection key; message-length limits are not part of
// the key), which the final fatal policy submit kills as well.
grpc_lite_phpt_assert_same(3, $prefaceCount, 'every call after a fatal ran on a fresh connection');

echo "OK\n";
?>
--EXPECT--
OK
