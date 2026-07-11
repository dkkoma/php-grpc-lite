--TEST--
grpc a connection killed under one call is terminal for other in-flight streams (no I/O re-drive)
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50066]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-034.jsonl
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

// Fixture :50066 answers the first request with one message and keeps the
// stream open; a second request on the same connection closes the TCP
// connection abruptly.
$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50066', $opts);
$client = new GreeterClient('test-server:50066', $opts, $channel);

// 1. deadline-less stream A: read the first message, stream stays open.
$streamCall = $client->BenchServerStream(new BenchRequest());
$responses = $streamCall->responses();
grpc_lite_phpt_assert_true($responses->current() !== null, 'stream A first message');

// 2. unary B on the shared connection: the server kills the TCP connection.
[, $status] = $client->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $status->code, 'unary B status after connection kill');

// 3. stream A's next pull must terminate promptly without re-driving I/O on
// the dead session (a deadline-less call must not block forever, and no
// bytes may be emitted after a potentially partial frame).
$responses->next();
grpc_lite_phpt_assert_same(false, $responses->valid(), 'stream A yields no further messages');
$streamStatus = $streamCall->getStatus();
// Client-observed connection break after the response started maps to
// UNAVAILABLE per the gRPC status taxonomy, with the transport failure
// snapshotted into the details.
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $streamStatus->code, 'stream A terminal status is UNAVAILABLE');
grpc_lite_phpt_assert_true(is_string($streamStatus->details) && $streamStatus->details !== '', 'stream A terminal details carry the transport failure');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$unaryEndUs = null;
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_kind'] ?? null) === 'unary') {
        $unaryEndUs = $record['monotonic_us'] ?? null;
    }
}
grpc_lite_phpt_assert_true($unaryEndUs !== null, 'unary rpc.end exists');

// No socket/TLS I/O may happen after the unary observed the dead connection:
// stream A's final pull must bail out on the terminal guard, not re-drive
// nghttp2 or the socket.
$ioEvents = ['wire.socket_write', 'wire.tls_write', 'wire.tls_write_retry', 'wire.socket_read', 'wire.tls_read', 'wire.tls_read_retry', 'wire.frame_out'];
foreach ($records as $record) {
    if (in_array($record['event'] ?? null, $ioEvents, true) && ($record['monotonic_us'] ?? 0) > $unaryEndUs) {
        throw new RuntimeException('unexpected I/O after connection death: ' . json_encode($record));
    }
}

echo "OK\n";
?>
--EXPECT--
OK
