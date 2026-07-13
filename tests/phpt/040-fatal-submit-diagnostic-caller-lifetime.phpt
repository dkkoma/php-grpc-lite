--TEST--
grpc_lite_unary diagnostic caller does not touch the connection consumed by a fatal submit
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
GRPC_LITE_TEST_FAULT=submit-request-fatal
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Helloworld\HelloRequest;

// Lifetime contract regression (ASan-relevant): a fatal nghttp2_submit_request
// makes the unary core detach the connection from the persistent cache and
// destroy it before returning FAILURE. The diagnostic grpc_lite_unary() caller
// holds the raw connection pointer across that call; it must not dereference
// it afterwards (the original code called connection_usable() on the freed
// pointer -> deterministic heap-use-after-free under ASan).
$hello = new HelloRequest();
$hello->setName('DiagnosticFatal');
$request = $hello->serializeToString();

for ($attempt = 0; $attempt < 2; $attempt++) {
    grpc_lite_phpt_expect_throw(
        static function () use ($request): void {
            grpc_lite_unary('diag-key', 'test-server', 50051, '/helloworld.Greeter/SayHello', $request);
        },
        'nghttp2_submit_request failed',
    );
}

echo "OK\n";
?>
--EXPECT--
OK
