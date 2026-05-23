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
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_ext_grpc_158_settings_profile'), 'official settings experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_data_chunk_window_update'), 'response window update experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_wait_initial_settings_ack'), 'wait settings ack experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_ext_grpc_158_wire_profile'), 'ext-grpc wire profile experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_hpack_deflate_table_size_zero'), 'hpack deflate table size zero experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_ext_grpc_158_header_padding_target'), 'ext-grpc wire profile header padding target ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_add_grpc_accept_encoding'), 'grpc accept encoding experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_user_agent_extra_bytes'), 'user agent extra bytes experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_split_x_goog_api_client'), 'split x-goog-api-client experiment ini');
grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.http2_experimental_no_index_x_bench_padding'), 'no-index x-bench-padding experiment ini');
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
