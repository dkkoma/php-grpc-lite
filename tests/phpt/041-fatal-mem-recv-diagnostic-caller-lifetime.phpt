--TEST--
grpc_lite_unary diagnostic caller does not touch the connection consumed by a fatal mem_recv
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
grpc_lite_phpt_skip_if_test_fault_seam_unavailable();
grpc_lite_phpt_skip_if_bench_diagnostics_unavailable();
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-041.jsonl
GRPC_LITE_TEST_FAULT=rst-submit-fatal
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Helloworld\BenchRequest;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

// Lifetime contract regression for the mem_recv-fatal FAILURE branch: the
// response exceeds max_receive_message_length, the policy RST submitted
// inside the session callback hits the fatal seam, the callback fails and
// nghttp2_session_mem_recv() returns fatal. The unary core must consume the
// connection (detach + destroy) before throwing, and the diagnostic
// grpc_lite_unary() caller must not dereference the pointer afterwards
// (ASan-relevant, same shape as PHPT 040 but through the mem_recv branch).
$bench = new BenchRequest();
$bench->setPayloadBytes(1024);
$request = $bench->serializeToString();
$call = static function (?string $authority) use ($request): void {
    grpc_lite_unary(
        'diag-key',
        'test-server',
        50051,
        '/helloworld.Greeter/BenchUnary',
        $request,
        [],
        0,
        false,
        null,
        null,
        null,
        8,
        $authority,
    );
};

for ($attempt = 0; $attempt < 2; $attempt++) {
    grpc_lite_phpt_expect_throw(
        static function () use ($call): void {
            $call(null);
        },
        'nghttp2_session_mem_recv failed',
    );
}

// Cache retention regression: the consumed connection must not stay in the
// persistent cache. With lazy per-key eviction only, 128+ distinct keys
// would fill the cache and later calls would fail with "persistent
// connection cache limit exceeded" instead of the mem_recv failure.
for ($i = 0; $i < 130; $i++) {
    $throwable = grpc_lite_phpt_expect_throw(
        static function () use ($call, $i): void {
            $call("authority-$i.test");
        },
    );
    grpc_lite_phpt_assert_contains('nghttp2_session_mem_recv failed', $throwable->getMessage(), "fatal #$i is a mem_recv failure, not cache exhaustion");
}

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$prefaceCount = 0;
$closeCount = 0;
foreach ($lines as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $closeCount++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        throw new RuntimeException('no RST_STREAM may reach the wire after a fatal submit: ' . $line);
    }
}
// Each attempt found the previous connection destroyed (never reused): 2
// initial attempts + 130 distinct-key sweep attempts.
grpc_lite_phpt_assert_same(132, $prefaceCount, 'a consumed connection was never reused');
// ...and every consumed connection was actually destroyed during the run,
// not merely detached from the cache (a detach-only regression would leak
// all 132 connections invisibly: same exceptions, same preface count, and
// ASan runs with leak detection disabled).
grpc_lite_phpt_assert_same(132, $closeCount, 'every consumed connection was destroyed');

echo "OK\n";
?>
--EXPECT--
OK
