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

foreach ([
    'grpc_lite_unary',
    'grpc_lite_server_streaming_open',
    'grpc_lite_server_streaming_next',
    'grpc_lite_server_streaming_cancel',
    'grpc_lite_multiplex_unary',
    'grpc_lite_bench_unary_batch',
] as $function) {
    grpc_lite_phpt_assert_true(!function_exists($function), "$function must not be exposed in production build");
}

echo "OK\n";
?>
--EXPECT--
OK
