/*
 * Bench-only native transport entrypoints.
 *
 * This file is included from grpc.c intentionally. The benchmark harness uses
 * the same low-level HTTP/2 helpers and diagnostics as the production native
 * transport, but those helpers should remain file-local instead of becoming a
 * wider extension ABI.
 */

PHP_FUNCTION(grpc_native_bench_unary_batch)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zend_long iterations = 0;
    zval *headers_zv = NULL;
    bool split_grpc_frame = false;
    bool no_copy = false;
    bool poll_loop = false;
    bool discard_response_body = false;
    zend_long data_frame_size = 0;
    zend_long recv_stream_window_size = 0;
    zend_long recv_connection_window_size = 0;
    zend_long recv_buffer_size = 16384;
    bool flush_after_mem_recv = false;
    bool read_first_poll_loop = false;
    bool decode_response_incrementally = false;
    bool direct_response_payload = false;
    bool read_ahead_delivery = false;
    zend_long read_ahead_max_messages = 0;
    zend_long read_ahead_max_bytes = 0;
    zend_long timeout_us = 0;
    bool compact_response_buffer = false;
    zend_long response_compact_threshold = 0;
    zval *response_callback_zv = NULL;
    bool response_callback_enabled = false;
    zend_fcall_info response_fci;
    zend_fcall_info_cache response_fcc;
    grpc_call client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    char authority[512];
    int rv;
    char recv_buf[MAX_RECV_BUF_SIZE];
    size_t recv_buf_len = 16384;
    zend_long ok = 0;
    zend_long failed = 0;
    uint64_t total_started;
    uint64_t total_elapsed;
    zval latencies;
    zval client_first_data_sent_us;
    zval client_upload_complete_us;
    zval client_first_response_data_us;
    zval client_last_response_data_us;
    zval client_first_window_update_us;
    zval client_last_window_update_us;
    zval client_first_window_update_sent_us;
    zval client_last_window_update_sent_us;
    zval client_first_flow_control_pause_us;
    zval client_response_header_us;
    zval client_stream_close_us;
    zval client_first_response_message_ready_us;
    zval client_last_response_message_ready_us;
    zval client_first_response_callback_done_us;
    zval client_last_response_callback_done_us;
    zval call_window_update_frames_recv;
    zval call_connection_window_update_frames_recv;
    zval call_stream_window_update_frames_recv;
    zval call_connection_window_update_increment_recv;
    zval call_stream_window_update_increment_recv;
    zval call_window_update_frames_sent;
    zval call_connection_window_update_frames_sent;
    zval call_stream_window_update_frames_sent;
    zval call_connection_window_update_increment_sent;
    zval call_stream_window_update_increment_sent;
    zval call_data_read_length_calls;
    zval call_flow_control_pauses;
    zval call_max_write_syscall_us;
    zval call_recv_syscalls;
    zval call_recv_syscall_us;
    zval call_max_recv_syscall_us;
    zval call_mem_recv_us;
    zval call_max_mem_recv_us;
    zval call_session_send_after_recv_us;
    zval call_max_session_send_after_recv_us;
    zval call_poll_wait_us;
    zval call_max_poll_wait_us;
    zval call_pollin_ready;
    zval call_pollout_ready;
    zval call_poll_to_data_us;
    zval call_max_poll_to_data_us;
    zval call_window_update_to_data_us;
    zval call_max_window_update_to_data_us;
    zval call_receive_drains;
    zval call_receive_drains_with_data;
    zval call_receive_drains_eagain_after_data;
    zval call_max_reads_per_drain;
    zval call_max_bytes_per_drain;
    zval call_min_session_remote_window;
    zval call_min_stream_remote_window;
    zval call_response_data_bytes;
    zval call_data_recv_calls;
    zval call_body_append_us;
    zval call_max_body_append_us;
    zval call_body_compact_count;
    zval call_body_compact_bytes;
    zval call_body_compact_us;
    zval call_max_body_compact_us;
    zval call_max_body_buffer_bytes;
    zval call_decoded_messages;
    zval call_max_response_queue_count;
    zval call_max_response_queue_bytes;
    zval call_response_queue_wait_us;
    zval call_max_response_queue_wait_us;
    zval call_response_payload_string_us;
    zval call_max_response_payload_string_us;
    zval call_response_decode_us;
    zval call_max_response_decode_us;
    zval server_handler_ns;
    zval server_payload_alloc_ns;
    zval server_payload_bytes;
    zval server_request_payload_bytes;
    zval server_stats_handler_start_ns;
    zval server_stats_handler_end_ns;
    zval server_stats_in_payload_ns;
    zval server_stats_out_header_ns;
    zval server_stats_out_payload_ns;
    zval server_stats_first_out_payload_ns;
    zval server_stats_last_out_payload_ns;
    zval server_stats_out_payload_count;
    zval server_stats_out_payload_bytes;
    zval server_stats_out_payload_wire_bytes;
    zval server_stats_out_payload_compressed_bytes;

    memset(&response_fci, 0, sizeof(response_fci));
    memset(&response_fcc, 0, sizeof(response_fcc));

    ZEND_PARSE_PARAMETERS_START(5, 25)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_LONG(iterations)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_BOOL(split_grpc_frame)
        Z_PARAM_BOOL(no_copy)
        Z_PARAM_LONG(data_frame_size)
        Z_PARAM_BOOL(poll_loop)
        Z_PARAM_BOOL(discard_response_body)
        Z_PARAM_LONG(recv_stream_window_size)
        Z_PARAM_LONG(recv_connection_window_size)
        Z_PARAM_LONG(recv_buffer_size)
        Z_PARAM_BOOL(flush_after_mem_recv)
        Z_PARAM_BOOL(read_first_poll_loop)
        Z_PARAM_ZVAL_OR_NULL(response_callback_zv)
        Z_PARAM_BOOL(decode_response_incrementally)
        Z_PARAM_BOOL(compact_response_buffer)
        Z_PARAM_LONG(response_compact_threshold)
        Z_PARAM_BOOL(direct_response_payload)
        Z_PARAM_BOOL(read_ahead_delivery)
        Z_PARAM_LONG(read_ahead_max_messages)
        Z_PARAM_LONG(read_ahead_max_bytes)
        Z_PARAM_LONG(timeout_us)
    ZEND_PARSE_PARAMETERS_END();

    if (iterations < 1) {
        zend_throw_exception(NULL, "iterations must be positive", 0);
        RETURN_THROWS();
    }
    if (response_callback_zv != NULL && Z_TYPE_P(response_callback_zv) != IS_NULL) {
        if (zend_fcall_info_init(response_callback_zv, 0, &response_fci, &response_fcc, NULL, NULL) != SUCCESS) {
            zend_throw_exception(NULL, "response callback must be callable", 0);
            RETURN_THROWS();
        }
        response_callback_enabled = true;
        discard_response_body = false;
    }
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    client.no_copy = no_copy;
    client.poll_loop = poll_loop;
    client.discard_response_body = discard_response_body;
    client.flush_after_mem_recv = flush_after_mem_recv;
    client.read_first_poll_loop = read_first_poll_loop;
    client.decode_response_incrementally = decode_response_incrementally;
    client.direct_response_payload = direct_response_payload && decode_response_incrementally && response_callback_enabled;
    client.read_ahead_delivery = read_ahead_delivery && client.direct_response_payload;
    client.read_ahead_max_messages = read_ahead_max_messages > 0 ? (size_t) read_ahead_max_messages : 0;
    client.read_ahead_max_bytes = read_ahead_max_bytes > 0 ? (size_t) read_ahead_max_bytes : 0;
    client.compact_response_buffer = compact_response_buffer && decode_response_incrementally && !client.direct_response_payload;
    client.response_compact_threshold = response_compact_threshold > 0 ? (size_t) response_compact_threshold : 1;
    if (response_callback_enabled) {
        client.response_fci = &response_fci;
        client.response_fcc = &response_fcc;
    }
    if (data_frame_size > 0) {
        client.data_frame_size_cap = (uint32_t) data_frame_size;
    }
    if (recv_stream_window_size > 0) {
        client.recv_stream_window_size = (uint32_t) recv_stream_window_size;
    }
    if (recv_connection_window_size > 0) {
        client.recv_connection_window_size = (uint32_t) recv_connection_window_size;
    }
    if (recv_buffer_size > 0) {
        recv_buf_len = (size_t) recv_buffer_size;
        if (recv_buf_len > MAX_RECV_BUF_SIZE) {
            recv_buf_len = MAX_RECV_BUF_SIZE;
        }
    }
    if (split_grpc_frame) {
        set_grpc_header(&client, request_len);
    }

    client.deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    client.fd = connect_tcp(host, port, client.deadline_abs_us);
    if (client.fd < 0) {
        zend_throw_exception(NULL, errno == ETIMEDOUT ? "native transport deadline exceeded" : "failed to connect", 0);
        RETURN_THROWS();
    }
    if (poll_loop && set_nonblocking(client.fd) != 0) {
        close(client.fd);
        zend_throw_exception(NULL, "failed to set nonblocking", 0);
        RETURN_THROWS();
    }

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);
    nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, data_source_read_length_callback);
    nghttp2_session_callbacks_set_send_data_callback(callbacks, send_data_callback);
    nghttp2_session_client_new(&session, callbacks, &client);
    if (client.recv_stream_window_size > 0) {
        nghttp2_settings_entry iv[1] = {
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, client.recv_stream_window_size},
        };
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);
    } else {
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    }
    if (client.recv_connection_window_size > 65535) {
        nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE, 0, (int32_t) (client.recv_connection_window_size - 65535));
    }

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    init_request_headers(&request_headers, count_custom_header_values(headers_zv));
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, "http", sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, authority, strlen(authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    append_custom_request_headers(&request_headers, headers_zv);

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    array_init(&latencies);
    array_init(&client_first_data_sent_us);
    array_init(&client_upload_complete_us);
    array_init(&client_first_response_data_us);
    array_init(&client_last_response_data_us);
    array_init(&client_first_window_update_us);
    array_init(&client_last_window_update_us);
    array_init(&client_first_window_update_sent_us);
    array_init(&client_last_window_update_sent_us);
    array_init(&client_first_flow_control_pause_us);
    array_init(&client_response_header_us);
    array_init(&client_stream_close_us);
    array_init(&client_first_response_message_ready_us);
    array_init(&client_last_response_message_ready_us);
    array_init(&client_first_response_callback_done_us);
    array_init(&client_last_response_callback_done_us);
    array_init(&call_window_update_frames_recv);
    array_init(&call_connection_window_update_frames_recv);
    array_init(&call_stream_window_update_frames_recv);
    array_init(&call_connection_window_update_increment_recv);
    array_init(&call_stream_window_update_increment_recv);
    array_init(&call_window_update_frames_sent);
    array_init(&call_connection_window_update_frames_sent);
    array_init(&call_stream_window_update_frames_sent);
    array_init(&call_connection_window_update_increment_sent);
    array_init(&call_stream_window_update_increment_sent);
    array_init(&call_data_read_length_calls);
    array_init(&call_flow_control_pauses);
    array_init(&call_max_write_syscall_us);
    array_init(&call_recv_syscalls);
    array_init(&call_recv_syscall_us);
    array_init(&call_max_recv_syscall_us);
    array_init(&call_mem_recv_us);
    array_init(&call_max_mem_recv_us);
    array_init(&call_session_send_after_recv_us);
    array_init(&call_max_session_send_after_recv_us);
    array_init(&call_poll_wait_us);
    array_init(&call_max_poll_wait_us);
    array_init(&call_pollin_ready);
    array_init(&call_pollout_ready);
    array_init(&call_poll_to_data_us);
    array_init(&call_max_poll_to_data_us);
    array_init(&call_window_update_to_data_us);
    array_init(&call_max_window_update_to_data_us);
    array_init(&call_receive_drains);
    array_init(&call_receive_drains_with_data);
    array_init(&call_receive_drains_eagain_after_data);
    array_init(&call_max_reads_per_drain);
    array_init(&call_max_bytes_per_drain);
    array_init(&call_min_session_remote_window);
    array_init(&call_min_stream_remote_window);
    array_init(&call_response_data_bytes);
    array_init(&call_data_recv_calls);
    array_init(&call_body_append_us);
    array_init(&call_max_body_append_us);
    array_init(&call_body_compact_count);
    array_init(&call_body_compact_bytes);
    array_init(&call_body_compact_us);
    array_init(&call_max_body_compact_us);
    array_init(&call_max_body_buffer_bytes);
    array_init(&call_decoded_messages);
    array_init(&call_max_response_queue_count);
    array_init(&call_max_response_queue_bytes);
    array_init(&call_response_queue_wait_us);
    array_init(&call_max_response_queue_wait_us);
    array_init(&call_response_payload_string_us);
    array_init(&call_max_response_payload_string_us);
    array_init(&call_response_decode_us);
    array_init(&call_max_response_decode_us);
    array_init(&server_handler_ns);
    array_init(&server_payload_alloc_ns);
    array_init(&server_payload_bytes);
    array_init(&server_request_payload_bytes);
    array_init(&server_stats_handler_start_ns);
    array_init(&server_stats_handler_end_ns);
    array_init(&server_stats_in_payload_ns);
    array_init(&server_stats_out_header_ns);
    array_init(&server_stats_out_payload_ns);
    array_init(&server_stats_first_out_payload_ns);
    array_init(&server_stats_last_out_payload_ns);
    array_init(&server_stats_out_payload_count);
    array_init(&server_stats_out_payload_bytes);
    array_init(&server_stats_out_payload_wire_bytes);
    array_init(&server_stats_out_payload_compressed_bytes);
    total_started = monotonic_us();

    for (zend_long i = 0; i < iterations; i++) {
        uint64_t started = monotonic_us();
        client.call_started_us = started;
        client.deadline_abs_us = timeout_us > 0 ? started + (uint64_t) timeout_us : 0;
        client.stream_closed = false;
        client.grpc_status = -1;
        client.http_status = -1;
        client.stream_error_code = 0;
        client.compressed_response_seen = false;
        client.response_current_compressed = false;
        client.timed_out = false;
        client.request_offset = 0;
        client.pending_data_len = 0;
        clear_pending_write(&client);
        client.first_data_sent_us = 0;
        client.last_data_sent_us = 0;
        client.first_response_data_us = 0;
        client.last_response_data_us = 0;
        client.first_window_update_us = 0;
        client.last_window_update_us = 0;
        client.first_window_update_sent_us = 0;
        client.last_window_update_sent_us = 0;
        client.first_flow_control_pause_us = 0;
        client.first_response_header_us = 0;
        client.stream_closed_us = 0;
        client.first_response_message_ready_us = 0;
        client.last_response_message_ready_us = 0;
        client.first_response_callback_done_us = 0;
        client.last_response_callback_done_us = 0;
        client.call_window_update_frames_recv = 0;
        client.call_connection_window_update_frames_recv = 0;
        client.call_stream_window_update_frames_recv = 0;
        client.call_connection_window_update_increment_recv = 0;
        client.call_stream_window_update_increment_recv = 0;
        client.call_window_update_frames_sent = 0;
        client.call_connection_window_update_frames_sent = 0;
        client.call_stream_window_update_frames_sent = 0;
        client.call_connection_window_update_increment_sent = 0;
        client.call_stream_window_update_increment_sent = 0;
        client.call_data_read_length_calls = 0;
        client.call_flow_control_pauses = 0;
        client.call_max_write_syscall_us = 0;
        client.call_recv_syscalls = 0;
        client.call_recv_syscall_us = 0;
        client.call_max_recv_syscall_us = 0;
        client.call_mem_recv_us = 0;
        client.call_max_mem_recv_us = 0;
        client.call_session_send_after_recv_us = 0;
        client.call_max_session_send_after_recv_us = 0;
        client.call_poll_wait_us = 0;
        client.call_max_poll_wait_us = 0;
        client.call_pollin_ready = 0;
        client.call_pollout_ready = 0;
        client.call_poll_to_data_us = 0;
        client.call_max_poll_to_data_us = 0;
        client.call_window_update_to_data_us = 0;
        client.call_max_window_update_to_data_us = 0;
        client.call_receive_drains = 0;
        client.call_receive_drains_with_data = 0;
        client.call_receive_drains_eagain_after_data = 0;
        client.call_max_reads_per_drain = 0;
        client.call_max_bytes_per_drain = 0;
        client.last_poll_return_abs_us = 0;
        client.awaiting_data_after_poll = false;
        client.last_window_update_sent_abs_us = 0;
        client.awaiting_data_after_window_update_sent = false;
        client.call_min_session_remote_window = 0;
        client.call_min_stream_remote_window = 0;
        client.call_response_data_bytes = 0;
        client.call_data_recv_calls = 0;
        client.call_body_append_us = 0;
        client.call_max_body_append_us = 0;
        client.call_body_compact_count = 0;
        client.call_body_compact_bytes = 0;
        client.call_body_compact_us = 0;
        client.call_max_body_compact_us = 0;
        client.call_max_body_buffer_bytes = 0;
        client.response_parse_offset = 0;
        client.response_header_len = 0;
        client.response_payload_len = 0;
        client.response_payload_offset = 0;
        if (client.response_payload != NULL) {
            zend_string_release(client.response_payload);
            client.response_payload = NULL;
        }
        client.call_decoded_messages = 0;
        client.call_max_response_queue_count = 0;
        client.call_max_response_queue_bytes = 0;
        client.call_response_queue_wait_us = 0;
        client.call_max_response_queue_wait_us = 0;
        free_queued_response_payloads(&client);
        client.call_response_payload_string_us = 0;
        client.call_max_response_payload_string_us = 0;
        client.call_response_decode_us = 0;
        client.call_max_response_decode_us = 0;
        client.server_handler_ns = 0;
        client.server_payload_alloc_ns = 0;
        client.server_payload_bytes = 0;
        client.server_request_payload_bytes = 0;
        client.server_stats_handler_start_ns = 0;
        client.server_stats_handler_end_ns = 0;
        client.server_stats_in_payload_ns = 0;
        client.server_stats_out_header_ns = 0;
        client.server_stats_out_payload_ns = 0;
        client.server_stats_first_out_payload_ns = 0;
        client.server_stats_last_out_payload_ns = 0;
        client.server_stats_out_payload_count = 0;
        client.server_stats_out_payload_bytes = 0;
        client.server_stats_out_payload_wire_bytes = 0;
        client.server_stats_out_payload_compressed_bytes = 0;
        cleanup_grpc_call(&client);
        memset(&client.body, 0, sizeof(client.body));

        client.stream_id = nghttp2_submit_request(session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
        if (client.stream_id < 0) {
            failed++;
            break;
        }

        if (poll_loop) {
            rv = drive_stream_poll(session, &client, recv_buf, recv_buf_len);
            if (rv != 0) {
                failed++;
                break;
            }
        } else {
            rv = nghttp2_session_send(session);
            if (rv != 0) {
                failed++;
                break;
            }

            while (!client.stream_closed) {
                ssize_t nread = recv(client.fd, recv_buf, recv_buf_len, 0);
                if (nread <= 0) {
                    if (nread < 0) {
                        client.last_io_errno = errno;
                        snprintf(client.last_io_error_detail, sizeof(client.last_io_error_detail), "recv failed: %s", strerror(errno));
                    }
                    failed++;
                    break;
                }
                client.bytes_received += (size_t) nread;
                rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
                if (rv < 0) {
                    failed++;
                    break;
                }
                rv = nghttp2_session_send(session);
                if (rv != 0) {
                    failed++;
                    break;
                }
                client.last_session_error = rv;
            }
        }

        zend_long decoded_messages = client.call_decoded_messages;
        uint64_t response_payload_string_us = client.call_response_payload_string_us;
        uint64_t max_response_payload_string_us = client.call_max_response_payload_string_us;
        uint64_t response_decode_us = client.call_response_decode_us;
        uint64_t max_response_decode_us = client.call_max_response_decode_us;
        if (response_callback_enabled && !decode_response_incrementally) {
            if (process_response_messages(&client, &response_fci, &response_fcc, &decoded_messages, &response_decode_us, &max_response_decode_us) != 0) {
                failed++;
                break;
            }
        } else if (response_callback_enabled && decode_response_incrementally && client.direct_response_payload && (client.response_header_len != 0 || client.response_payload != NULL || client.response_queue_head != NULL)) {
            failed++;
            break;
        } else if (response_callback_enabled && decode_response_incrementally && !client.direct_response_payload && client.response_parse_offset != (client.body.s ? ZSTR_LEN(client.body.s) : 0)) {
            failed++;
            break;
        }

        add_next_index_long(&latencies, (zend_long) (monotonic_us() - started));
        add_next_index_long(&client_first_data_sent_us, (zend_long) client.first_data_sent_us);
        add_next_index_long(&client_upload_complete_us, (zend_long) client.last_data_sent_us);
        add_next_index_long(&client_first_response_data_us, (zend_long) client.first_response_data_us);
        add_next_index_long(&client_last_response_data_us, (zend_long) client.last_response_data_us);
        add_next_index_long(&client_first_window_update_us, (zend_long) client.first_window_update_us);
        add_next_index_long(&client_last_window_update_us, (zend_long) client.last_window_update_us);
        add_next_index_long(&client_first_window_update_sent_us, (zend_long) client.first_window_update_sent_us);
        add_next_index_long(&client_last_window_update_sent_us, (zend_long) client.last_window_update_sent_us);
        add_next_index_long(&client_first_flow_control_pause_us, (zend_long) client.first_flow_control_pause_us);
        add_next_index_long(&client_response_header_us, (zend_long) client.first_response_header_us);
        add_next_index_long(&client_stream_close_us, (zend_long) client.stream_closed_us);
        add_next_index_long(&client_first_response_message_ready_us, (zend_long) client.first_response_message_ready_us);
        add_next_index_long(&client_last_response_message_ready_us, (zend_long) client.last_response_message_ready_us);
        add_next_index_long(&client_first_response_callback_done_us, (zend_long) client.first_response_callback_done_us);
        add_next_index_long(&client_last_response_callback_done_us, (zend_long) client.last_response_callback_done_us);
        add_next_index_long(&call_window_update_frames_recv, (zend_long) client.call_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_frames_recv, (zend_long) client.call_connection_window_update_frames_recv);
        add_next_index_long(&call_stream_window_update_frames_recv, (zend_long) client.call_stream_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_increment_recv, (zend_long) client.call_connection_window_update_increment_recv);
        add_next_index_long(&call_stream_window_update_increment_recv, (zend_long) client.call_stream_window_update_increment_recv);
        add_next_index_long(&call_window_update_frames_sent, (zend_long) client.call_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_frames_sent, (zend_long) client.call_connection_window_update_frames_sent);
        add_next_index_long(&call_stream_window_update_frames_sent, (zend_long) client.call_stream_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_increment_sent, (zend_long) client.call_connection_window_update_increment_sent);
        add_next_index_long(&call_stream_window_update_increment_sent, (zend_long) client.call_stream_window_update_increment_sent);
        add_next_index_long(&call_data_read_length_calls, (zend_long) client.call_data_read_length_calls);
        add_next_index_long(&call_flow_control_pauses, (zend_long) client.call_flow_control_pauses);
        add_next_index_long(&call_max_write_syscall_us, (zend_long) client.call_max_write_syscall_us);
        add_next_index_long(&call_recv_syscalls, (zend_long) client.call_recv_syscalls);
        add_next_index_long(&call_recv_syscall_us, (zend_long) client.call_recv_syscall_us);
        add_next_index_long(&call_max_recv_syscall_us, (zend_long) client.call_max_recv_syscall_us);
        add_next_index_long(&call_mem_recv_us, (zend_long) client.call_mem_recv_us);
        add_next_index_long(&call_max_mem_recv_us, (zend_long) client.call_max_mem_recv_us);
        add_next_index_long(&call_session_send_after_recv_us, (zend_long) client.call_session_send_after_recv_us);
        add_next_index_long(&call_max_session_send_after_recv_us, (zend_long) client.call_max_session_send_after_recv_us);
        add_next_index_long(&call_poll_wait_us, (zend_long) client.call_poll_wait_us);
        add_next_index_long(&call_max_poll_wait_us, (zend_long) client.call_max_poll_wait_us);
        add_next_index_long(&call_pollin_ready, (zend_long) client.call_pollin_ready);
        add_next_index_long(&call_pollout_ready, (zend_long) client.call_pollout_ready);
        add_next_index_long(&call_poll_to_data_us, (zend_long) client.call_poll_to_data_us);
        add_next_index_long(&call_max_poll_to_data_us, (zend_long) client.call_max_poll_to_data_us);
        add_next_index_long(&call_window_update_to_data_us, (zend_long) client.call_window_update_to_data_us);
        add_next_index_long(&call_max_window_update_to_data_us, (zend_long) client.call_max_window_update_to_data_us);
        add_next_index_long(&call_receive_drains, (zend_long) client.call_receive_drains);
        add_next_index_long(&call_receive_drains_with_data, (zend_long) client.call_receive_drains_with_data);
        add_next_index_long(&call_receive_drains_eagain_after_data, (zend_long) client.call_receive_drains_eagain_after_data);
        add_next_index_long(&call_max_reads_per_drain, (zend_long) client.call_max_reads_per_drain);
        add_next_index_long(&call_max_bytes_per_drain, (zend_long) client.call_max_bytes_per_drain);
        add_next_index_long(&call_min_session_remote_window, (zend_long) client.call_min_session_remote_window);
        add_next_index_long(&call_min_stream_remote_window, (zend_long) client.call_min_stream_remote_window);
        add_next_index_long(&call_response_data_bytes, (zend_long) client.call_response_data_bytes);
        add_next_index_long(&call_data_recv_calls, (zend_long) client.call_data_recv_calls);
        add_next_index_long(&call_body_append_us, (zend_long) client.call_body_append_us);
        add_next_index_long(&call_max_body_append_us, (zend_long) client.call_max_body_append_us);
        add_next_index_long(&call_body_compact_count, (zend_long) client.call_body_compact_count);
        add_next_index_long(&call_body_compact_bytes, (zend_long) client.call_body_compact_bytes);
        add_next_index_long(&call_body_compact_us, (zend_long) client.call_body_compact_us);
        add_next_index_long(&call_max_body_compact_us, (zend_long) client.call_max_body_compact_us);
        add_next_index_long(&call_max_body_buffer_bytes, (zend_long) client.call_max_body_buffer_bytes);
        add_next_index_long(&call_decoded_messages, decoded_messages);
        add_next_index_long(&call_max_response_queue_count, (zend_long) client.call_max_response_queue_count);
        add_next_index_long(&call_max_response_queue_bytes, (zend_long) client.call_max_response_queue_bytes);
        add_next_index_long(&call_response_queue_wait_us, (zend_long) client.call_response_queue_wait_us);
        add_next_index_long(&call_max_response_queue_wait_us, (zend_long) client.call_max_response_queue_wait_us);
        add_next_index_long(&call_response_payload_string_us, (zend_long) response_payload_string_us);
        add_next_index_long(&call_max_response_payload_string_us, (zend_long) max_response_payload_string_us);
        add_next_index_long(&call_response_decode_us, (zend_long) response_decode_us);
        add_next_index_long(&call_max_response_decode_us, (zend_long) max_response_decode_us);
        add_next_index_long(&server_handler_ns, client.server_handler_ns);
        add_next_index_long(&server_payload_alloc_ns, client.server_payload_alloc_ns);
        add_next_index_long(&server_payload_bytes, client.server_payload_bytes);
        add_next_index_long(&server_request_payload_bytes, client.server_request_payload_bytes);
        add_next_index_long(&server_stats_handler_start_ns, client.server_stats_handler_start_ns);
        add_next_index_long(&server_stats_handler_end_ns, client.server_stats_handler_end_ns);
        add_next_index_long(&server_stats_in_payload_ns, client.server_stats_in_payload_ns);
        add_next_index_long(&server_stats_out_header_ns, client.server_stats_out_header_ns);
        add_next_index_long(&server_stats_out_payload_ns, client.server_stats_out_payload_ns);
        add_next_index_long(&server_stats_first_out_payload_ns, client.server_stats_first_out_payload_ns);
        add_next_index_long(&server_stats_last_out_payload_ns, client.server_stats_last_out_payload_ns);
        add_next_index_long(&server_stats_out_payload_count, client.server_stats_out_payload_count);
        add_next_index_long(&server_stats_out_payload_bytes, client.server_stats_out_payload_bytes);
        add_next_index_long(&server_stats_out_payload_wire_bytes, client.server_stats_out_payload_wire_bytes);
        add_next_index_long(&server_stats_out_payload_compressed_bytes, client.server_stats_out_payload_compressed_bytes);
        if (client.stream_closed && client.grpc_status == 0 && client.http_status == 200) {
            ok++;
        } else {
            failed++;
            break;
        }
    }

    total_elapsed = monotonic_us() - total_started;

    close(client.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    free_request_headers(&request_headers);

    array_init(return_value);
    add_assoc_long(return_value, "iterations", iterations);
    add_assoc_long(return_value, "ok", ok);
    add_assoc_long(return_value, "failed", failed);
    add_assoc_long(return_value, "total_us", (zend_long) total_elapsed);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
    add_assoc_long(return_value, "body_bytes", client.body.s ? ZSTR_LEN(client.body.s) : 0);
    add_assoc_bool(return_value, "discard_response_body", discard_response_body);
    add_assoc_bool(return_value, "split_grpc_frame", split_grpc_frame);
    add_assoc_bool(return_value, "no_copy", no_copy);
    add_assoc_bool(return_value, "poll_loop", poll_loop);
    add_assoc_bool(return_value, "flush_after_mem_recv", flush_after_mem_recv);
    add_assoc_bool(return_value, "read_first_poll_loop", read_first_poll_loop);
    add_assoc_bool(return_value, "response_callback_enabled", response_callback_enabled);
    add_assoc_bool(return_value, "decode_response_incrementally", decode_response_incrementally);
    add_assoc_bool(return_value, "direct_response_payload", client.direct_response_payload);
    add_assoc_bool(return_value, "read_ahead_delivery", client.read_ahead_delivery);
    add_assoc_bool(return_value, "timed_out", client.timed_out);
    add_assoc_long(return_value, "last_io_errno", client.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", client.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", client.last_io_error_detail);
    add_assoc_bool(return_value, "compact_response_buffer", client.compact_response_buffer);
    add_assoc_long(return_value, "response_compact_threshold", (zend_long) client.response_compact_threshold);
    add_assoc_long(return_value, "request_wire_bytes", client.grpc_header_len + client.request_len);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "response_data_bytes", client.response_data_bytes);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_long(return_value, "data_frames_sent", client.data_frames_sent);
    add_assoc_long(return_value, "data_bytes_sent", client.data_bytes_sent);
    add_assoc_long(return_value, "window_update_frames_recv", client.window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_frames_recv", client.connection_window_update_frames_recv);
    add_assoc_long(return_value, "stream_window_update_frames_recv", client.stream_window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_increment_recv", client.connection_window_update_increment_recv);
    add_assoc_long(return_value, "stream_window_update_increment_recv", client.stream_window_update_increment_recv);
    add_assoc_long(return_value, "window_update_frames_sent", client.window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_frames_sent", client.connection_window_update_frames_sent);
    add_assoc_long(return_value, "stream_window_update_frames_sent", client.stream_window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_increment_sent", client.connection_window_update_increment_sent);
    add_assoc_long(return_value, "stream_window_update_increment_sent", client.stream_window_update_increment_sent);
    add_assoc_long(return_value, "flow_control_pauses", client.flow_control_pauses);
    add_assoc_long(return_value, "send_callback_calls", client.send_callback_calls);
    add_assoc_long(return_value, "send_data_callback_calls", client.send_data_callback_calls);
    add_assoc_long(return_value, "write_syscalls", client.write_syscalls);
    add_assoc_long(return_value, "send_wouldblock_calls", client.send_wouldblock_calls);
    add_assoc_long(return_value, "recv_wouldblock_calls", client.recv_wouldblock_calls);
    add_assoc_long(return_value, "poll_calls", client.poll_calls);
    add_assoc_long(return_value, "poll_timeouts", client.poll_timeouts);
    add_assoc_long(return_value, "poll_errors", client.poll_errors);
    add_assoc_long(return_value, "max_write_syscall_us", client.max_write_syscall_us);
    add_assoc_long(return_value, "data_read_calls", client.data_read_calls);
    add_assoc_long(return_value, "data_read_length_calls", client.data_read_length_calls);
    add_assoc_long(return_value, "data_recv_calls", client.data_recv_calls);
    add_assoc_long(return_value, "max_send_callback_len", client.max_send_callback_len);
    add_assoc_long(return_value, "max_data_frame_len", client.max_data_frame_len);
    add_assoc_long(return_value, "min_data_frame_len", client.min_data_frame_len);
    add_assoc_long(return_value, "max_read_len", client.max_read_len);
    add_assoc_long(return_value, "min_read_len", client.min_read_len);
    add_assoc_long(return_value, "data_frame_size_cap", client.data_frame_size_cap);
    add_assoc_long(return_value, "recv_stream_window_size", client.recv_stream_window_size);
    add_assoc_long(return_value, "recv_connection_window_size", client.recv_connection_window_size);
    add_assoc_long(return_value, "recv_buffer_size", (zend_long) recv_buf_len);
    add_assoc_long(return_value, "min_session_remote_window", client.min_session_remote_window);
    add_assoc_long(return_value, "min_stream_remote_window", client.min_stream_remote_window);
    add_assoc_long(return_value, "remote_max_frame_size", client.remote_max_frame_size);
    add_assoc_zval(return_value, "latencies_us", &latencies);
    add_assoc_zval(return_value, "client_first_data_sent_us", &client_first_data_sent_us);
    add_assoc_zval(return_value, "client_upload_complete_us", &client_upload_complete_us);
    add_assoc_zval(return_value, "client_first_response_data_us", &client_first_response_data_us);
    add_assoc_zval(return_value, "client_last_response_data_us", &client_last_response_data_us);
    add_assoc_zval(return_value, "client_first_window_update_us", &client_first_window_update_us);
    add_assoc_zval(return_value, "client_last_window_update_us", &client_last_window_update_us);
    add_assoc_zval(return_value, "client_first_window_update_sent_us", &client_first_window_update_sent_us);
    add_assoc_zval(return_value, "client_last_window_update_sent_us", &client_last_window_update_sent_us);
    add_assoc_zval(return_value, "client_first_flow_control_pause_us", &client_first_flow_control_pause_us);
    add_assoc_zval(return_value, "client_response_header_us", &client_response_header_us);
    add_assoc_zval(return_value, "client_stream_close_us", &client_stream_close_us);
    add_assoc_zval(return_value, "client_first_response_message_ready_us", &client_first_response_message_ready_us);
    add_assoc_zval(return_value, "client_last_response_message_ready_us", &client_last_response_message_ready_us);
    add_assoc_zval(return_value, "client_first_response_callback_done_us", &client_first_response_callback_done_us);
    add_assoc_zval(return_value, "client_last_response_callback_done_us", &client_last_response_callback_done_us);
    add_assoc_zval(return_value, "call_window_update_frames_recv", &call_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_frames_recv", &call_connection_window_update_frames_recv);
    add_assoc_zval(return_value, "call_stream_window_update_frames_recv", &call_stream_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_increment_recv", &call_connection_window_update_increment_recv);
    add_assoc_zval(return_value, "call_stream_window_update_increment_recv", &call_stream_window_update_increment_recv);
    add_assoc_zval(return_value, "call_window_update_frames_sent", &call_window_update_frames_sent);
    add_assoc_zval(return_value, "call_connection_window_update_frames_sent", &call_connection_window_update_frames_sent);
    add_assoc_zval(return_value, "call_stream_window_update_frames_sent", &call_stream_window_update_frames_sent);
    add_assoc_zval(return_value, "call_connection_window_update_increment_sent", &call_connection_window_update_increment_sent);
    add_assoc_zval(return_value, "call_stream_window_update_increment_sent", &call_stream_window_update_increment_sent);
    add_assoc_zval(return_value, "call_data_read_length_calls", &call_data_read_length_calls);
    add_assoc_zval(return_value, "call_flow_control_pauses", &call_flow_control_pauses);
    add_assoc_zval(return_value, "call_max_write_syscall_us", &call_max_write_syscall_us);
    add_assoc_zval(return_value, "call_recv_syscalls", &call_recv_syscalls);
    add_assoc_zval(return_value, "call_recv_syscall_us", &call_recv_syscall_us);
    add_assoc_zval(return_value, "call_max_recv_syscall_us", &call_max_recv_syscall_us);
    add_assoc_zval(return_value, "call_mem_recv_us", &call_mem_recv_us);
    add_assoc_zval(return_value, "call_max_mem_recv_us", &call_max_mem_recv_us);
    add_assoc_zval(return_value, "call_session_send_after_recv_us", &call_session_send_after_recv_us);
    add_assoc_zval(return_value, "call_max_session_send_after_recv_us", &call_max_session_send_after_recv_us);
    add_assoc_zval(return_value, "call_poll_wait_us", &call_poll_wait_us);
    add_assoc_zval(return_value, "call_max_poll_wait_us", &call_max_poll_wait_us);
    add_assoc_zval(return_value, "call_pollin_ready", &call_pollin_ready);
    add_assoc_zval(return_value, "call_pollout_ready", &call_pollout_ready);
    add_assoc_zval(return_value, "call_poll_to_data_us", &call_poll_to_data_us);
    add_assoc_zval(return_value, "call_max_poll_to_data_us", &call_max_poll_to_data_us);
    add_assoc_zval(return_value, "call_window_update_to_data_us", &call_window_update_to_data_us);
    add_assoc_zval(return_value, "call_max_window_update_to_data_us", &call_max_window_update_to_data_us);
    add_assoc_zval(return_value, "call_receive_drains", &call_receive_drains);
    add_assoc_zval(return_value, "call_receive_drains_with_data", &call_receive_drains_with_data);
    add_assoc_zval(return_value, "call_receive_drains_eagain_after_data", &call_receive_drains_eagain_after_data);
    add_assoc_zval(return_value, "call_max_reads_per_drain", &call_max_reads_per_drain);
    add_assoc_zval(return_value, "call_max_bytes_per_drain", &call_max_bytes_per_drain);
    add_assoc_zval(return_value, "call_min_session_remote_window", &call_min_session_remote_window);
    add_assoc_zval(return_value, "call_min_stream_remote_window", &call_min_stream_remote_window);
    add_assoc_zval(return_value, "call_response_data_bytes", &call_response_data_bytes);
    add_assoc_zval(return_value, "call_data_recv_calls", &call_data_recv_calls);
    add_assoc_zval(return_value, "call_body_append_us", &call_body_append_us);
    add_assoc_zval(return_value, "call_max_body_append_us", &call_max_body_append_us);
    add_assoc_zval(return_value, "call_body_compact_count", &call_body_compact_count);
    add_assoc_zval(return_value, "call_body_compact_bytes", &call_body_compact_bytes);
    add_assoc_zval(return_value, "call_body_compact_us", &call_body_compact_us);
    add_assoc_zval(return_value, "call_max_body_compact_us", &call_max_body_compact_us);
    add_assoc_zval(return_value, "call_max_body_buffer_bytes", &call_max_body_buffer_bytes);
    add_assoc_zval(return_value, "call_decoded_messages", &call_decoded_messages);
    add_assoc_zval(return_value, "call_max_response_queue_count", &call_max_response_queue_count);
    add_assoc_zval(return_value, "call_max_response_queue_bytes", &call_max_response_queue_bytes);
    add_assoc_zval(return_value, "call_response_queue_wait_us", &call_response_queue_wait_us);
    add_assoc_zval(return_value, "call_max_response_queue_wait_us", &call_max_response_queue_wait_us);
    add_assoc_zval(return_value, "call_response_payload_string_us", &call_response_payload_string_us);
    add_assoc_zval(return_value, "call_max_response_payload_string_us", &call_max_response_payload_string_us);
    add_assoc_zval(return_value, "call_response_decode_us", &call_response_decode_us);
    add_assoc_zval(return_value, "call_max_response_decode_us", &call_max_response_decode_us);
    add_assoc_zval(return_value, "server_handler_ns", &server_handler_ns);
    add_assoc_zval(return_value, "server_payload_alloc_ns", &server_payload_alloc_ns);
    add_assoc_zval(return_value, "server_payload_bytes", &server_payload_bytes);
    add_assoc_zval(return_value, "server_request_payload_bytes", &server_request_payload_bytes);
    add_assoc_zval(return_value, "server_stats_handler_start_ns", &server_stats_handler_start_ns);
    add_assoc_zval(return_value, "server_stats_handler_end_ns", &server_stats_handler_end_ns);
    add_assoc_zval(return_value, "server_stats_in_payload_ns", &server_stats_in_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_header_ns", &server_stats_out_header_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_ns", &server_stats_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_first_out_payload_ns", &server_stats_first_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_last_out_payload_ns", &server_stats_last_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_count", &server_stats_out_payload_count);
    add_assoc_zval(return_value, "server_stats_out_payload_bytes", &server_stats_out_payload_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_wire_bytes", &server_stats_out_payload_wire_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_compressed_bytes", &server_stats_out_payload_compressed_bytes);
    cleanup_grpc_call(&client);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_native_bench_unary_batch, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, iterations, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, split_grpc_frame, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, no_copy, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, data_frame_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, poll_loop, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, discard_response_body, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_stream_window_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_connection_window_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_buffer_size, IS_LONG, 0, "16384")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flush_after_mem_recv, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_first_poll_loop, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, response_callback, IS_CALLABLE, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, decode_response_incrementally, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, compact_response_buffer, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, response_compact_threshold, IS_LONG, 0, "1")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, direct_response_payload, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_delivery, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_max_messages, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_max_bytes, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()
