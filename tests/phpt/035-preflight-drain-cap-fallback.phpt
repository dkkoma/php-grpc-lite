--TEST--
grpc preflight drain cap: large cancelled-stream backlog falls back to a fresh connection, follow-up call succeeds
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-035.jsonl
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

// Flood the stream well past the 64KiB preflight drain cap (stream window is
// 8MiB, the server sends without pacing), read one message, then cancel.
$request = new BenchRequest();
$request->setMessageCount(200);
$request->setPayloadBytes(65536);
$call = $client->BenchServerStream($request, []);
$responses = $call->responses();
grpc_lite_phpt_assert_true($responses->current() !== null, 'first streamed message');
$call->cancel();

// Connection reuse is best-effort: with more backlog than the drain cap the
// adoption preflight gives up and opens a fresh connection. The follow-up
// call must succeed either way.
$hello = new HelloRequest();
$hello->setName('AfterBacklog');
[$response, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'follow-up unary status');
grpc_lite_phpt_assert_same('Hello, AfterBacklog', $response->getMessage(), 'follow-up unary reply');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$followUpEnd = null;
$prefaceCount = 0;
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello') {
        $followUpEnd = $record;
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
}

// Pin today's best-effort fallback: the cap is exceeded, so the follow-up
// runs on a second connection. If bounded adoption ever lands, update this
// together with SPEC §4.2.
grpc_lite_phpt_assert_true(is_array($followUpEnd), 'follow-up rpc.end exists');
grpc_lite_phpt_assert_same(false, $followUpEnd['persistent_reused'] ?? null, 'follow-up fell back to a fresh connection');
grpc_lite_phpt_assert_same(2, $prefaceCount, 'exactly two connections were opened');

echo "OK\n";
?>
--EXPECT--
OK
