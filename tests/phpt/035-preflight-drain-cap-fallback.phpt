--TEST--
grpc preflight drain cap: large cancelled-stream backlog falls back to a fresh connection, follow-up call succeeds
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50068, 50069]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-035.jsonl
--INI--
grpc_lite.preflight_drain_max_bytes=16384
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

// Fixture :50068 (control :50069): "arm" marks the next connection as the
// flood target whose first stream gets one message and stays open; "flood"
// writes 48KiB of DATA for that stream with a tiny SO_SNDBUF and replies
// "ready" only once the client's TCP stack has ACKed (received) nearly all
// of it. That is the deterministic barrier proving the backlog sits in the
// client kernel before the connection is adopted. The backlog must fit the
// client's TCP receive window (~64KiB with default kernel settings), so the
// drain cap is lowered below the backlog via ini instead of flooding past
// the production 64KiB cap, which the kernel window cannot hold.
$control = fsockopen('test-server', 50069, $errno, $errstr, 5.0);
grpc_lite_phpt_assert_true(is_resource($control), 'control channel connected');
fwrite($control, "arm\n");
grpc_lite_phpt_assert_same("armed\n", fgets($control), 'fixture armed');

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50068', $opts);
$client = new GreeterClient('test-server:50068', $opts, $channel);

$call = $client->BenchServerStream(new BenchRequest(), []);
$responses = $call->responses();
grpc_lite_phpt_assert_true($responses->current() !== null, 'first streamed message');

fwrite($control, "flood\n");
stream_set_timeout($control, 10);
grpc_lite_phpt_assert_same("ready\n", fgets($control), 'backlog resident in client kernel');
fclose($control);

$call->cancel();

// Connection reuse is best-effort: with more backlog than the 64KiB drain
// cap the adoption preflight gives up (draining) and opens a fresh
// connection. The follow-up call must succeed either way.
$hello = new HelloRequest();
$hello->setName('AfterBacklog');
[, $status] = $client->SayHello($hello)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'follow-up unary status');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$followUpEnd = null;
$prefaceCount = 0;
$preflightBytes = 0;
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello') {
        $followUpEnd = $record;
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
    if (($record['event'] ?? null) === 'wire.socket_preflight_read' && ($record['result_len'] ?? 0) > 0) {
        $preflightBytes += $record['result_len'];
    }
}

// The drain must actually have hit its cap (proving the backlog was there),
// and only then is the fresh-connection fallback the pinned expectation. If
// bounded adoption ever lands, update this together with SPEC §4.2.
grpc_lite_phpt_assert_true($preflightBytes >= 16384, 'preflight drain read up to its cap');
grpc_lite_phpt_assert_true($preflightBytes <= 16384, 'preflight drain never reads past its cap');
grpc_lite_phpt_assert_true(is_array($followUpEnd), 'follow-up rpc.end exists');
grpc_lite_phpt_assert_same(false, $followUpEnd['persistent_reused'] ?? null, 'follow-up fell back to a fresh connection');
grpc_lite_phpt_assert_same(2, $prefaceCount, 'exactly two connections were opened');

echo "OK\n";
?>
--EXPECT--
OK
