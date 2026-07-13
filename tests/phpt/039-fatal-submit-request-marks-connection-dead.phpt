--TEST--
grpc fatal nghttp2_submit_request (fault injection) marks the connection dead and it is never reused
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
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-039.jsonl
GRPC_LITE_TEST_FAULT=submit-request-fatal
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

// Every nghttp2_submit_request in this process simulates a fatal
// NGHTTP2_ERR_NOMEM. The fatal must mark the connection dead so it is never
// handed to a later call (a second attempt must open a fresh connection
// instead of reusing the corrupted session).
$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);

$hello = new HelloRequest();
$hello->setName('Fatal');
grpc_lite_phpt_expect_throw(
    static function () use ($client, $hello): void {
        $client->SayHello($hello)->wait();
    },
    'nghttp2_submit_request failed',
);
grpc_lite_phpt_expect_throw(
    static function () use ($client, $hello): void {
        $client->SayHello($hello)->wait();
    },
    'nghttp2_submit_request failed',
);

$streamCall = $client->BenchServerStream(new BenchRequest(), []);
foreach ($streamCall->responses() as $_reply) {
}
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $streamCall->getStatus()->code, 'streaming setup failure status');

// Cache retention regression: every fatal must evict its dead cache entry
// immediately. With lazy per-key eviction only, 128+ distinct connection
// keys would fill the persistent cache and later calls would fail with
// "persistent connection cache limit exceeded" instead of the submit fault.
$hello2 = new HelloRequest();
$hello2->setName('CacheSweep');
for ($i = 0; $i < 130; $i++) {
    $keyedClient = new GreeterClient('test-server:50051', [
        'credentials' => ChannelCredentials::createInsecure(),
        'grpc.default_authority' => "authority-$i.test",
    ]);
    $throwable = grpc_lite_phpt_expect_throw(
        static function () use ($keyedClient, $hello2): void {
            $keyedClient->SayHello($hello2)->wait();
        },
    );
    grpc_lite_phpt_assert_contains('nghttp2_submit_request failed', $throwable->getMessage(), "fatal #$i is a submit failure, not cache exhaustion");
}

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$prefaceCount = 0;
foreach ($lines as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out' && in_array($record['frame_type'] ?? null, ['HEADERS', 'DATA', 'RST_STREAM'], true)) {
        throw new RuntimeException('no stream frames may reach the wire after a fatal submit: ' . $line);
    }
}
// Each attempt found the previous connection dead (or evicted) and opened a
// fresh one: 3 initial attempts + 130 distinct-key sweep attempts.
grpc_lite_phpt_assert_same(133, $prefaceCount, 'a dead connection was never reused');

echo "OK\n";
?>
--EXPECT--
OK
