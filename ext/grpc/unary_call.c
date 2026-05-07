/* Unary gRPC client call execution over an HTTP/2 connection. Included by main.c. */

#include "internal.h"
static int grpc_lite_unary_call_perform_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, zval *return_value)
{
    grpc_call call;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    int rv;
    char recv_buf[16384];
    uint64_t total_started = 0;
    uint64_t setup_started = 0;
    uint64_t setup_us = 0;
    uint64_t submit_started = 0;
    uint64_t submit_us = 0;
    uint64_t initial_send_started = 0;
    uint64_t initial_send_us = 0;
    uint64_t recv_loop_started = 0;
    uint64_t recv_loop_us = 0;
    grpc_lite_status_result status_result;
    if (!connection_usable(connection)) {
        zend_throw_exception(NULL, "invalid grpc_lite connection", 0);
        return FAILURE;
    }
    if (connection->busy) {
        zend_throw_exception(NULL, "HTTP/2 connection already has an active stream", 0);
        return FAILURE;
    }
    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }

    memset(&call, 0, sizeof(call));
    call.fd = connection->fd;
    call.connection = connection;
    call.grpc_status = -1;
    call.http_status = -1;
    call.max_response_messages = 1;
    call.request = (const uint8_t *) request;
    call.request_len = request_len;
    call.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    call.max_response_metadata_bytes = max_response_metadata_bytes;
    grpc_protocol_set_message_header(&call, call.request_len);
    total_started = monotonic_us();
    call.deadline_abs_us = timeout_us > 0 ? total_started + (uint64_t) timeout_us : 0;
    if (set_socket_timeout_us(connection->fd, timeout_us) != 0) {
        mark_connection_dead(connection, errno);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    setup_started = monotonic_us();
    connection->busy = true;
    // cppcheck-suppress autoVariables
    connection->active_call = &call;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        clear_connection_call_owner(connection, &call);
        cleanup_grpc_call(&call);
        return FAILURE;
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, connection->tls ? "https" : "http", connection->tls ? sizeof("https") - 1 : sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, connection->authority, strlen(connection->authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    append_grpc_timeout_request_header(&request_headers, timeout_us);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        clear_connection_call_owner(connection, &call);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    setup_us = monotonic_us() - setup_started;

    submit_started = monotonic_us();
    call.stream_id = nghttp2_submit_request(connection->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    submit_us = monotonic_us() - submit_started;
    if (call.stream_id < 0) {
        clear_connection_call_owner(connection, &call);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    initial_send_started = monotonic_us();
    rv = nghttp2_session_send(connection->session);
    initial_send_us = monotonic_us() - initial_send_started;
    if (rv != 0) {
        mark_connection_dead(connection, rv);
        recv_loop_us = 0;
        goto build_unary_result;
    }

    recv_loop_started = monotonic_us();
    while (!call.stream_closed) {
        ssize_t nread = connection_recv(connection, (uint8_t *) recv_buf, sizeof(recv_buf), call.deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && call.deadline_abs_us > 0;
            if (nread < 0) {
                call.last_io_errno = errno;
                call.last_ssl_error = connection->last_ssl_error;
                snprintf(call.last_io_error_detail, sizeof(call.last_io_error_detail), "%s", connection->last_error_detail);
            }
            if (socket_timeout) {
                call.timed_out = true;
            }
            if (!call.stream_closed) {
                mark_connection_dead(connection, nread == 0 ? 0 : errno);
            }
            break;
        }
        call.bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(connection->session, (const uint8_t *) recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_connection_dead(connection, rv);
            clear_connection_call_owner(connection, &call);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&call);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            return FAILURE;
        }
        rv = nghttp2_session_send(connection->session);
        if (rv != 0) {
            mark_connection_dead(connection, rv);
            break;
        }
        call.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;
build_unary_result:
    clear_connection_call_owner(connection, &call);
    resolve_grpc_call_status(&call, false, &status_result);

    array_init(return_value);
    add_status_result_to_return(return_value, &status_result);
    smart_str_0(&call.body);
    add_assoc_str(return_value, "body", call.body.s ? zend_string_copy(call.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", call.grpc_status);
    add_assoc_str(return_value, "grpc_message", call.grpc_message != NULL ? zend_string_copy(call.grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", call.http_status);
    add_assoc_long(return_value, "stream_error_code", call.stream_error_code);
    add_assoc_bool(return_value, "stream_reset_seen", call.stream_reset_seen);
    add_assoc_bool(return_value, "invalid_grpc_status", call.invalid_grpc_status);
    add_assoc_str(return_value, "content_type", call.content_type != NULL ? zend_string_copy(call.content_type) : zend_empty_string);
    add_assoc_str(return_value, "grpc_encoding", call.grpc_encoding != NULL ? zend_string_copy(call.grpc_encoding) : zend_empty_string);
    add_assoc_bool(return_value, "compressed_response_seen", call.compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", call.response_message_too_large);
    add_assoc_bool(return_value, "malformed_response_frame", call.malformed_response_frame);
    add_assoc_bool(return_value, "metadata_too_large", call.metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", call.invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", call.unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", call.max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) call.max_receive_message_bytes);
    add_assoc_long(return_value, "body_bytes", call.body.s ? ZSTR_LEN(call.body.s) : 0);
    add_assoc_long(return_value, "request_offset", call.request_offset);
    add_assoc_long(return_value, "bytes_sent", call.bytes_sent);
    add_assoc_long(return_value, "bytes_received", call.bytes_received);
    add_assoc_long(return_value, "data_read_calls", call.data_read_calls);
    add_assoc_long(return_value, "data_recv_calls", call.data_recv_calls);
    add_assoc_long(return_value, "last_session_error", call.last_session_error);
    add_assoc_long(return_value, "last_sent_frame_type", call.last_sent_frame_type);
    add_assoc_long(return_value, "last_recv_frame_type", call.last_recv_frame_type);
    add_assoc_long(return_value, "last_sent_frame_flags", call.last_sent_frame_flags);
    add_assoc_long(return_value, "last_recv_frame_flags", call.last_recv_frame_flags);
    add_assoc_long(return_value, "last_not_sent_frame_type", call.last_not_sent_frame_type);
    add_assoc_long(return_value, "last_not_sent_error", call.last_not_sent_error);
    add_assoc_long(return_value, "sent_frames", call.sent_frames);
    add_assoc_long(return_value, "recv_frames", call.recv_frames);
    add_assoc_long(return_value, "not_sent_frames", call.not_sent_frames);
    add_assoc_long(return_value, "last_io_errno", call.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", call.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", call.last_io_error_detail);
    add_assoc_long(return_value, "total_us", (zend_long) (monotonic_us() - total_started));
    add_assoc_long(return_value, "connect_us", 0);
    add_assoc_long(return_value, "setup_us", (zend_long) setup_us);
    add_assoc_long(return_value, "submit_us", (zend_long) submit_us);
    add_assoc_long(return_value, "initial_send_us", (zend_long) initial_send_us);
    add_assoc_long(return_value, "recv_loop_us", (zend_long) recv_loop_us);
    add_assoc_long(return_value, "cleanup_us", 0);
    add_assoc_bool(return_value, "timed_out", call.timed_out);
    add_assoc_bool(return_value, "channel_reused", connection_reused);
    add_assoc_bool(return_value, "persistent_reused", persistent_reused);
    add_assoc_bool(return_value, "connection_dead", connection->dead);
    add_assoc_bool(return_value, "connection_draining", connection->draining);
    add_assoc_bool(return_value, "connection_retired", connection->retired);
    add_assoc_long(return_value, "connection_last_error", connection->last_error);
    add_assoc_long(return_value, "connection_last_io_errno", connection->last_io_errno);
    add_assoc_long(return_value, "connection_last_ssl_error", connection->last_ssl_error);
    add_assoc_long(return_value, "connection_tls_verify_result", (zend_long) connection->tls_verify_result);
    add_assoc_string(return_value, "connection_last_error_detail", connection->last_error_detail);
    add_assoc_string(return_value, "connection_negotiated_protocol", connection->negotiated_protocol);
    add_assoc_long(return_value, "connection_last_goaway_error_code", connection->last_goaway_error_code);
    add_assoc_long(return_value, "connection_last_goaway_stream_id", connection->last_goaway_stream_id);
    grpc_protocol_add_metadata_map_to_return(return_value, "initial_metadata", &call, false);
    grpc_protocol_add_metadata_map_to_return(return_value, "trailing_metadata", &call, true);
    zend_string_release(status_result.details);
    free_request_headers(&request_headers);
    cleanup_grpc_call(&call);
    return SUCCESS;
}
