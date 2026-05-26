--TEST--
grpc extension registers HTTP/2 and server streaming INI defaults
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

grpc_lite_phpt_assert_same('8388608', ini_get('grpc_lite.http2_stream_window_size'), 'stream window ini');
grpc_lite_phpt_assert_same('8388608', ini_get('grpc_lite.http2_connection_window_size'), 'connection window ini');
grpc_lite_phpt_assert_same('16384', ini_get('grpc_lite.http2_max_frame_size'), 'max frame size ini');
grpc_lite_phpt_assert_same('65536', ini_get('grpc_lite.http2_max_header_list_size'), 'max header list size ini');
grpc_lite_phpt_assert_same('32', ini_get('grpc_lite.server_streaming_read_ahead_max_messages'), 'read-ahead messages ini');
grpc_lite_phpt_assert_same('8388608', ini_get('grpc_lite.server_streaming_read_ahead_max_bytes'), 'read-ahead bytes ini');
grpc_lite_phpt_assert_same('auto', ini_get('grpc_lite.backend'), 'backend ini');

grpc_lite_phpt_assert_true(ini_set('grpc_lite.server_streaming_read_ahead_max_messages', '4') !== false, 'read-ahead messages can be changed at runtime');
grpc_lite_phpt_assert_true(ini_set('grpc_lite.server_streaming_read_ahead_max_bytes', '262144') !== false, 'read-ahead bytes can be changed at runtime');
grpc_lite_phpt_assert_same('4', ini_get('grpc_lite.server_streaming_read_ahead_max_messages'), 'changed read-ahead messages ini');
grpc_lite_phpt_assert_same('262144', ini_get('grpc_lite.server_streaming_read_ahead_max_bytes'), 'changed read-ahead bytes ini');
grpc_lite_phpt_assert_true(ini_set('grpc_lite.server_streaming_read_ahead_max_messages', '-1') !== false, 'invalid read-ahead messages ini accepted then clamped');
grpc_lite_phpt_assert_true(ini_set('grpc_lite.server_streaming_read_ahead_max_bytes', '-1') !== false, 'invalid read-ahead bytes ini accepted then clamped');
grpc_lite_phpt_assert_same('-1', ini_get('grpc_lite.server_streaming_read_ahead_max_messages'), 'invalid read-ahead messages ini stored');
grpc_lite_phpt_assert_same('-1', ini_get('grpc_lite.server_streaming_read_ahead_max_bytes'), 'invalid read-ahead bytes ini stored');

echo "OK\n";
?>
--EXPECT--
OK
