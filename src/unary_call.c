/* Unary gRPC client call execution over an HTTP/2 connection. */

#include "diagnostic/diagnostic.h"

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static void grpc_lite_unary_add_diagnostic_result(zval *diagnostic_result, const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status_result, uint64_t start_unix_nanos, uint64_t total_started, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused)
{
    grpc_lite_diagnostic_add_unary_result(diagnostic_result, path, path_len, metadata, call, connection, status_result, start_unix_nanos, total_started > 0 ? monotonic_us() - total_started : 0, setup_us, submit_us, initial_send_us, recv_loop_us, connection_reused, persistent_reused);
    add_status_result_to_return(diagnostic_result, status_result);
    add_assoc_str(diagnostic_result, "body", call->body.s ? zend_string_copy(call->body.s) : zend_empty_string);
    add_assoc_str(diagnostic_result, "grpc_message", call->grpc_message != NULL ? zend_string_copy(call->grpc_message) : zend_empty_string);
    add_assoc_bool(diagnostic_result, "stream_refused_seen", call->stream_refused_seen);
    add_assoc_bool(diagnostic_result, "invalid_grpc_status", call->invalid_grpc_status);
    add_assoc_str(diagnostic_result, "content_type", call->content_type != NULL ? zend_string_copy(call->content_type) : zend_empty_string);
    add_assoc_str(diagnostic_result, "grpc_encoding", call->grpc_encoding != NULL ? zend_string_copy(call->grpc_encoding) : zend_empty_string);
    add_assoc_bool(diagnostic_result, "compressed_response_seen", call->compressed_response_seen);
    add_assoc_bool(diagnostic_result, "response_message_too_large", call->response_message_too_large);
    add_assoc_bool(diagnostic_result, "malformed_response_frame", call->malformed_response_frame);
    add_assoc_bool(diagnostic_result, "metadata_too_large", call->metadata_too_large);
    add_assoc_bool(diagnostic_result, "invalid_content_type", call->invalid_content_type);
    add_assoc_bool(diagnostic_result, "unsupported_response_encoding", call->unsupported_response_encoding);
    add_assoc_long(diagnostic_result, "max_receive_message_length", call->max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) call->max_receive_message_bytes);
    add_assoc_long(diagnostic_result, "request_offset", call->request_offset);
    add_assoc_long(diagnostic_result, "data_read_calls", call->data_read_calls);
    add_assoc_long(diagnostic_result, "data_recv_calls", call->data_recv_calls);
    add_assoc_long(diagnostic_result, "last_session_error", call->last_session_error);
    add_assoc_long(diagnostic_result, "last_sent_frame_type", call->last_sent_frame_type);
    add_assoc_long(diagnostic_result, "last_recv_frame_type", call->last_recv_frame_type);
    add_assoc_long(diagnostic_result, "last_sent_frame_flags", call->last_sent_frame_flags);
    add_assoc_long(diagnostic_result, "last_recv_frame_flags", call->last_recv_frame_flags);
    add_assoc_long(diagnostic_result, "last_not_sent_frame_type", call->last_not_sent_frame_type);
    add_assoc_long(diagnostic_result, "last_not_sent_error", call->last_not_sent_error);
    add_assoc_long(diagnostic_result, "not_sent_frames", call->not_sent_frames);
    add_assoc_long(diagnostic_result, "last_io_errno", call->last_io_errno);
    add_assoc_long(diagnostic_result, "last_ssl_error", call->last_ssl_error);
    add_assoc_string(diagnostic_result, "last_io_error_detail", call->last_io_error_detail);
    add_assoc_long(diagnostic_result, "connect_us", 0);
    add_assoc_long(diagnostic_result, "cleanup_us", 0);
    add_assoc_bool(diagnostic_result, "timed_out", call->timed_out);
    add_assoc_long(diagnostic_result, "connection_last_error", connection->last_error);
    add_assoc_long(diagnostic_result, "connection_last_io_errno", connection->last_io_errno);
    add_assoc_long(diagnostic_result, "connection_last_ssl_error", connection->last_ssl_error);
    add_assoc_long(diagnostic_result, "connection_tls_verify_result", (zend_long) connection->tls_verify_result);
    add_assoc_string(diagnostic_result, "connection_last_error_detail", connection->last_error_detail);
    add_assoc_string(diagnostic_result, "connection_negotiated_protocol", connection->negotiated_protocol);
    add_assoc_long(diagnostic_result, "connection_last_goaway_error_code", connection->last_goaway_error_code);
    add_assoc_long(diagnostic_result, "connection_last_goaway_stream_id", connection->last_goaway_stream_id);
    grpc_protocol_add_metadata_map_to_return(diagnostic_result, "initial_metadata", call, false);
    grpc_protocol_add_metadata_map_to_return(diagnostic_result, "trailing_metadata", call, true);
}
#endif

static int grpc_lite_unary_call_perform_core_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, uint64_t deadline_abs_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused,
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    zval *diagnostic_result,
#endif
    grpc_lite_unary_result *typed_result)
{
    grpc_call call;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers = {0};
    int rv;
    zend_long remaining_timeout_us;
    char recv_buf[16384];
    grpc_lite_status_result status_result;
    if (!connection_usable(connection)) {
        zend_throw_exception(NULL, "invalid grpc_lite connection", 0);
        return FAILURE;
    }
    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }

    memset(&call, 0, sizeof(call));
    call.connection = connection;
    call.grpc_status = -1;
    call.http_status = -1;
    call.max_response_messages = 1;
    call.request = (const uint8_t *) request;
    call.request_len = request_len;
    call.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    call.max_response_metadata_bytes = max_response_metadata_bytes;
    call.method_path = zend_string_init(path, path_len, 0);
    grpc_protocol_set_message_header(&call, call.request_len);
    call.deadline_abs_us = deadline_abs_us;
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        call.timed_out = true;
        goto build_unary_result;
    }

    if (init_request_headers(&request_headers) != 0) {
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
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        call.timed_out = true;
        clear_connection_call_owner(connection, &call);
        goto build_unary_result;
    }
    append_grpc_timeout_request_header(&request_headers, remaining_timeout_us);
    append_user_agent_request_header(&request_headers, primary_user_agent);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        clear_connection_call_owner(connection, &call);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    data_provider.source.ptr = &call;

    call.stream_id = nghttp2_submit_request(connection->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    if (call.stream_id < 0) {
        clear_connection_call_owner(connection, &call);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }
    grpc_lite_trace_request_headers(&call, request_headers.nva, request_headers.len);
    if (register_grpc_call_stream(connection, &call) != SUCCESS) {
        mark_grpc_call_stream_registration_failed(connection, &call);
        clear_connection_call_owner(connection, &call);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
        zend_throw_exception(NULL, "failed to register HTTP/2 stream", 0);
        return FAILURE;
    }

    rv = send_pending_h2_frames(connection, &call);
    if (rv != 0) {
        mark_connection_dead(connection, rv);
        goto build_unary_result;
    }

    while (!call.stream_closed) {
        ssize_t nread;
        connection->current_io_call = &call;
        nread = connection_recv(connection, (uint8_t *) recv_buf, sizeof(recv_buf), call.deadline_abs_us);
        connection->current_io_call = NULL;
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
        connection->current_read_call = &call;
        rv = nghttp2_session_mem_recv(connection->session, (const uint8_t *) recv_buf, (size_t) nread);
        connection->current_read_call = NULL;
        if (rv < 0) {
            mark_connection_dead(connection, rv);
            clear_connection_call_owner(connection, &call);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&call);
            destroy_detached_connection_if_unowned(connection);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            return FAILURE;
        }
        if (nghttp2_session_want_write(connection->session)) {
            rv = send_pending_h2_frames(connection, &call);
            if (rv != 0) {
                mark_connection_dead(connection, rv);
                break;
            }
        }
    }
	build_unary_result:
	    resolve_grpc_call_status(&call, false, &status_result);

    smart_str_0(&call.body);
    if (typed_result != NULL) {
        typed_result->body = call.body.s ? zend_string_copy(call.body.s) : zend_string_copy(zend_empty_string);
        typed_result->status.code = status_result.code;
        typed_result->status.details = zend_string_copy(status_result.details);
        grpc_protocol_copy_metadata_map(&typed_result->initial_metadata, &call, false);
        grpc_protocol_copy_metadata_map(&typed_result->trailing_metadata, &call, true);
    }
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
	    if (diagnostic_result != NULL) {
	        grpc_lite_unary_add_diagnostic_result(diagnostic_result, path, path_len, headers_zv, &call, connection, &status_result, 0, 0, 0, 0, 0, 0, connection_reused, persistent_reused);
	    }
#endif
	    clear_connection_call_owner(connection, &call);
	    zend_string_release(status_result.details);
	    free_request_headers(&request_headers);
	    cleanup_grpc_call(&call);
	    destroy_detached_connection_if_unowned(connection);
	    return SUCCESS;
	}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
int grpc_lite_unary_call_perform_diagnostic_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, zend_long timeout_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, zval *return_value)
{
    uint64_t deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    return grpc_lite_unary_call_perform_core_on_connection(connection, path, path_len, request, request_len, headers_zv, primary_user_agent, deadline_abs_us, max_receive_message_length, max_response_metadata_bytes, connection_reused, persistent_reused, return_value, NULL);
}
#endif

void grpc_lite_unary_result_dtor(grpc_lite_unary_result *result)
{
    if (result->body != NULL) {
        zend_string_release(result->body);
    }
    if (result->status.details != NULL) {
        zend_string_release(result->status.details);
    }
    zval_ptr_dtor(&result->initial_metadata);
    zval_ptr_dtor(&result->trailing_metadata);
}

int grpc_lite_unary_call_perform_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, uint64_t deadline_abs_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, grpc_lite_unary_result *result)
{
    memset(result, 0, sizeof(*result));
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    return grpc_lite_unary_call_perform_core_on_connection(connection, path, path_len, request, request_len, headers_zv, primary_user_agent, deadline_abs_us, max_receive_message_length, max_response_metadata_bytes, connection_reused, persistent_reused, NULL, result);
#else
    return grpc_lite_unary_call_perform_core_on_connection(connection, path, path_len, request, request_len, headers_zv, primary_user_agent, deadline_abs_us, max_receive_message_length, max_response_metadata_bytes, connection_reused, persistent_reused, result);
#endif
}
