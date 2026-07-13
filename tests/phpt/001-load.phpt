--TEST--
grpc extension can be loaded and exposes production module surface
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';

grpc_lite_phpt_assert_true(extension_loaded('grpc'), 'grpc extension must be loaded');
grpc_lite_phpt_assert_true(class_exists(Grpc\Channel::class), 'Grpc\\Channel must exist');
grpc_lite_phpt_assert_true(class_exists(Grpc\Call::class), 'Grpc\\Call must exist');
grpc_lite_phpt_assert_true(class_exists(Grpc\Timeval::class), 'Grpc\\Timeval must exist');
grpc_lite_phpt_assert_true(class_exists(Grpc\ChannelCredentials::class), 'Grpc\\ChannelCredentials must exist');
grpc_lite_phpt_assert_true(class_exists(Grpc\CallCredentials::class), 'Grpc\\CallCredentials must exist');

foreach ([
    'Grpc\\STATUS_OK' => 0,
    'Grpc\\STATUS_CANCELLED' => 1,
    'Grpc\\STATUS_DEADLINE_EXCEEDED' => 4,
    'Grpc\\STATUS_UNAVAILABLE' => 14,
    'Grpc\\OP_SEND_INITIAL_METADATA' => 0,
    'Grpc\\OP_SEND_MESSAGE' => 1,
    'Grpc\\OP_RECV_STATUS_ON_CLIENT' => 6,
] as $name => $expected) {
    grpc_lite_phpt_assert_same($expected, constant($name), $name);
}

// The bench diagnostic surface must match the lane's declared expectation:
// only runners that intentionally build --enable-grpc-bench export
// GRPC_LITE_EXPECT_BENCH=1; everything else (including a bare production
// run) expects non-exposure. The expectation is external input on purpose —
// deriving it from this module's own MINFO would also pass when a build-flag
// mistake exposes the bench surface in a production lane. The MINFO row must
// agree with the same external expectation.
$expectBench = getenv('GRPC_LITE_EXPECT_BENCH') === '1';
$expectTestFault = getenv('GRPC_LITE_EXPECT_TEST_FAULT') === '1';
ob_start();
phpinfo(INFO_MODULES);
$minfo = (string) ob_get_clean();
$minfoBench = str_contains($minfo, 'grpc_lite bench diagnostics');
$minfoTestFault = str_contains($minfo, 'grpc_lite test fault seam');
grpc_lite_phpt_assert_same($expectBench, $minfoBench, 'MINFO bench diagnostics row must match the lane expectation');
// Same external-oracle rule for the test fault seam: a production lane
// (GRPC_LITE_EXPECT_TEST_FAULT unset/0) must fail here if the seam leaked
// into the build, instead of silently un-skipping the fault PHPTs.
grpc_lite_phpt_assert_same($expectTestFault, $minfoTestFault, 'MINFO test fault seam row must match the lane expectation');
foreach ([
    'grpc_lite_unary',
    'grpc_lite_server_streaming_open',
    'grpc_lite_server_streaming_next',
    'grpc_lite_server_streaming_cancel',
    'grpc_lite_bench_unary_batch',
] as $function) {
    grpc_lite_phpt_assert_same(
        $expectBench,
        function_exists($function),
        $expectBench
            ? "$function must be exposed in a --enable-grpc-bench lane"
            : "$function must not be exposed in a production lane",
    );
}

echo "OK\n";
?>
--EXPECT--
OK
