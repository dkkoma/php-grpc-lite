--TEST--
grpc destroying a call with flow-control-deferred request DATA on a draining connection resets the stream before free
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50070]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-037.jsonl
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

// Fixture :50070 advertises INITIAL_WINDOW_SIZE=1024 and answers stream A
// with one message without consuming its large request body, so A's
// remaining request DATA stays flow-control deferred inside nghttp2 with
// the data provider pointing at A's call state. B gets one message plus a
// draining GOAWAY(MaxInt32); 500ms later the fixture opens A's window
// (which would resume A's deferred DATA if the stream were still open) and
// completes B. Destroying A while draining must therefore RST A's stream
// before freeing its state — the original use-after-free shape.
$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50070', $opts);
$client = new GreeterClient('test-server:50070', $opts, $channel);

$largeRequest = new BenchRequest();
$largeRequest->setRequestPayload(str_repeat("\0", 262144));
$streamA = $client->BenchServerStream($largeRequest, []);
$aResponses = $streamA->responses();
grpc_lite_phpt_assert_true($aResponses->current() !== null, 'A first message (stream open, request DATA deferred)');

$streamB = $client->BenchServerStream(new BenchRequest(), []);
$bResponses = $streamB->responses();
grpc_lite_phpt_assert_true($bResponses->current() !== null, 'B first message');

// Connection is draining now (GOAWAY arrived with B's first message).
// Destroy A: its deferred request DATA must be detached via RST_STREAM.
unset($aResponses, $streamA);

// B keeps driving the shared session across the fixture's delayed
// WINDOW_UPDATE for A; with A's stream reset this must be a no-op.
$bResponses->next();
grpc_lite_phpt_assert_true($bResponses->current() !== null, 'B second message');
$bResponses->next();
grpc_lite_phpt_assert_same(false, $bResponses->valid(), 'B stream complete');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $streamB->getStatus()->code, 'B status');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$goAwayUs = null;
$rstFrame = null;
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'wire.frame_in' && ($record['frame_type'] ?? null) === 'GOAWAY') {
        $goAwayUs = $record['monotonic_us'] ?? null;
    }
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        grpc_lite_phpt_assert_true($rstFrame === null, 'exactly one RST_STREAM');
        $rstFrame = $record;
    }
}
grpc_lite_phpt_assert_true($goAwayUs !== null, 'GOAWAY observed');
grpc_lite_phpt_assert_true(is_array($rstFrame), 'destructor sent RST_STREAM on the draining connection');
grpc_lite_phpt_assert_same(8, $rstFrame['error_code'] ?? null, 'RST_STREAM error code is CANCEL');
grpc_lite_phpt_assert_true(($rstFrame['monotonic_us'] ?? 0) > $goAwayUs, 'RST_STREAM sent after draining started');

echo "OK\n";
?>
--EXPECT--
OK
