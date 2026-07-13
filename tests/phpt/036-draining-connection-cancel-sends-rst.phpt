--TEST--
grpc cancel on a draining connection still sends RST_STREAM for the admitted stream
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50067]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-036.jsonl
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

// Fixture :50067 answers the first request with one message, then sends a
// draining-only GOAWAY(MaxInt32) and keeps the admitted stream open without
// trailers.
$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50067', $opts);
$client = new GreeterClient('test-server:50067', $opts, $channel);

$call = $client->BenchServerStream(new BenchRequest(), []);
$responses = $call->responses();
grpc_lite_phpt_assert_true($responses->current() !== null, 'first streamed message');

// Explicit cancel while the connection is draining: the admitted stream must
// still be closed with RST_STREAM(CANCEL) on the wire before the call state
// is released — skipping it would leave nghttp2 holding the freed call as
// the stream's data provider.
$call->cancel();
grpc_lite_phpt_assert_same(Grpc\STATUS_CANCELLED, $call->getStatus()->code, 'cancelled status');

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
        $rstFrame = $record;
    }
}

grpc_lite_phpt_assert_true($goAwayUs !== null, 'GOAWAY observed');
grpc_lite_phpt_assert_true(is_array($rstFrame), 'RST_STREAM sent on the draining connection');
grpc_lite_phpt_assert_same(8, $rstFrame['error_code'] ?? null, 'RST_STREAM error code is CANCEL');
grpc_lite_phpt_assert_true(($rstFrame['monotonic_us'] ?? 0) > $goAwayUs, 'RST_STREAM was sent after the connection started draining');

echo "OK\n";
?>
--EXPECT--
OK
