/* Server streaming gRPC client call execution over an HTTP/2 connection. Included by main.c. */

#include "internal.h"

static void server_streaming_call_terminate_with_cancel(server_streaming_call_state *state)
{
    if (state == NULL) {
        return;
    }
    free_queued_response_payloads(&state->call);
    if (connection_owned_by_server_streaming_call_state(state->call.connection, state) && connection_usable(state->call.connection) && state->call.stream_id > 0) {
        int rv = nghttp2_submit_rst_stream(state->call.connection->session, NGHTTP2_FLAG_NONE, state->call.stream_id, NGHTTP2_CANCEL);
        if (rv == 0) {
            rv = send_pending_h2_frames(state->call.connection, &state->call);
        }
        if (rv != 0) {
            mark_connection_dead(state->call.connection, rv);
            detach_persistent_connection_by_ptr(state->call.connection);
        }
    }
    state->completed = true;
}
static int server_streaming_call_open_resource(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, zend_long max_receive_message_length, size_t max_response_metadata_bytes, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, zval *return_value, grpc_lite_status_result *setup_failure)
{
    h2_connection *connection;
    server_streaming_call_state *state;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;
    uint64_t start_unix_nanos = unix_time_nanos();
    uint64_t total_started_us = monotonic_us();
    uint64_t setup_started_us = 0;
    uint64_t submit_started_us = 0;
    uint64_t initial_send_started_us = 0;
    int rv;

    error_message = validate_channel_inputs(key, key_len, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        return FAILURE;
    }
    if (timeout_us < 0) {
        zend_throw_exception(NULL, "timeout must be non-negative microseconds", 0);
        return FAILURE;
    }
    error_message = validate_grpc_path(path, path_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        return FAILURE;
    }

    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    setup_started_us = monotonic_us();
    connection = get_persistent_connection(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (connection == NULL) {
        if (setup_failure != NULL) {
            bool deadline_exceeded = (deadline_abs_us > 0 && monotonic_us() >= deadline_abs_us)
                || (error_message != NULL && strcmp(error_message, "HTTP/2 transport deadline exceeded") == 0);
            setup_failure->code = deadline_exceeded ? GRPC_STATUS_DEADLINE_EXCEEDED : GRPC_STATUS_UNAVAILABLE;
            setup_failure->details = zend_string_init(error_message != NULL ? error_message : "failed to open persistent connection", strlen(error_message != NULL ? error_message : "failed to open persistent connection"), 0);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent connection", 0);
        return FAILURE;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        if (setup_failure != NULL) {
            setup_failure->code = GRPC_STATUS_DEADLINE_EXCEEDED;
            setup_failure->details = zend_string_init("HTTP/2 transport deadline exceeded", sizeof("HTTP/2 transport deadline exceeded") - 1, 0);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        return FAILURE;
    }
    if (set_socket_timeout_us(connection->fd, remaining_timeout_us) != 0) {
        mark_connection_dead(connection, errno);
        discard_persistent_connection(key, key_len, connection);
        if (setup_failure != NULL) {
            setup_failure->code = GRPC_STATUS_UNAVAILABLE;
            setup_failure->details = zend_string_init("failed to set socket timeout", sizeof("failed to set socket timeout") - 1, 0);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    state = ecalloc(1, sizeof(server_streaming_call_state));
    state->request = zend_string_init(request, request_len, 0);
    state->path = zend_string_init(path, path_len, 0);
    ZVAL_COPY(&state->metadata, headers_zv);
    state->recv_buf_len = 65536;
    state->recv_buf = emalloc(state->recv_buf_len);
    state->start_unix_nanos = start_unix_nanos;
    state->total_started_us = total_started_us;
    state->setup_us = monotonic_us() - setup_started_us;
    state->persistent_reused = persistent_reused;

    memset(&state->call, 0, sizeof(state->call));
    state->call.connection = connection;
    state->call.grpc_status = -1;
    state->call.http_status = -1;
    state->call.request = (const uint8_t *) ZSTR_VAL(state->request);
    state->call.request_len = ZSTR_LEN(state->request);
    state->call.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    state->call.max_response_metadata_bytes = max_response_metadata_bytes;
    state->call.deadline_abs_us = deadline_abs_us > 0 ? deadline_abs_us : 0;
    state->call.decode_response_incrementally = true;
    state->call.direct_response_payload = true;
    state->call.queue_response_payloads = true;
    grpc_protocol_set_message_header(&state->call, state->call.request_len);

    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        destroy_server_streaming_call_state(state);
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
        if (setup_failure != NULL) {
            setup_failure->code = GRPC_STATUS_DEADLINE_EXCEEDED;
            setup_failure->details = zend_string_init("HTTP/2 transport deadline exceeded", sizeof("HTTP/2 transport deadline exceeded") - 1, 0);
            free_request_headers(&request_headers);
            destroy_server_streaming_call_state(state);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        return FAILURE;
    }
    append_grpc_timeout_request_header(&request_headers, remaining_timeout_us);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    data_provider.source.ptr = &state->call;
    submit_started_us = monotonic_us();
    state->call.stream_id = nghttp2_submit_request(connection->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    state->submit_us = monotonic_us() - submit_started_us;
    if (state->call.stream_id < 0) {
        if (setup_failure != NULL) {
            setup_failure->code = GRPC_STATUS_UNAVAILABLE;
            setup_failure->details = zend_string_init("nghttp2_submit_request failed", sizeof("nghttp2_submit_request failed") - 1, 0);
            free_request_headers(&request_headers);
            destroy_server_streaming_call_state(state);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }
    if (register_grpc_call_stream(connection, &state->call) != SUCCESS) {
        mark_grpc_call_stream_registration_failed(connection, &state->call);
        if (setup_failure != NULL) {
            setup_failure->code = GRPC_STATUS_UNAVAILABLE;
            setup_failure->details = zend_string_init("failed to register HTTP/2 stream", sizeof("failed to register HTTP/2 stream") - 1, 0);
            free_request_headers(&request_headers);
            destroy_server_streaming_call_state(state);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        zend_throw_exception(NULL, "failed to register HTTP/2 stream", 0);
        return FAILURE;
    }

    initial_send_started_us = monotonic_us();
    rv = send_pending_h2_frames(connection, &state->call);
    state->initial_send_us = monotonic_us() - initial_send_started_us;
    if (rv != 0) {
        bool stream_timed_out = state->call.timed_out;
        mark_connection_dead(connection, rv);
        if (setup_failure != NULL) {
            setup_failure->code = stream_timed_out ? GRPC_STATUS_DEADLINE_EXCEEDED : GRPC_STATUS_UNAVAILABLE;
            setup_failure->details = zend_string_init(stream_timed_out ? "HTTP/2 transport deadline exceeded" : "nghttp2_session_send failed", strlen(stream_timed_out ? "HTTP/2 transport deadline exceeded" : "nghttp2_session_send failed"), 0);
            free_request_headers(&request_headers);
            destroy_server_streaming_call_state(state);
            ZVAL_UNDEF(return_value);
            return SUCCESS;
        }
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        if (stream_timed_out) {
            zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
            return FAILURE;
        }
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        return FAILURE;
    }

    free_request_headers(&request_headers);
    ZVAL_RES(return_value, zend_register_resource(state, le_server_streaming_call_state));
    return SUCCESS;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static void server_streaming_call_add_status(zval *return_value, server_streaming_call_state *state)
{
    grpc_call *call = &state->call;
    grpc_lite_status_result status_result;
    resolve_grpc_call_status(call, state->cancelled, &status_result);
    grpc_lite_diagnostic_add_server_streaming_status(return_value, state, &status_result);
    add_assoc_bool(return_value, "done", true);
    add_status_result_to_return(return_value, &status_result);
    add_assoc_str(return_value, "grpc_message", call->grpc_message != NULL ? zend_string_copy(call->grpc_message) : zend_empty_string);
    add_assoc_bool(return_value, "stream_refused_seen", call->stream_refused_seen);
    add_assoc_bool(return_value, "invalid_grpc_status", call->invalid_grpc_status);
    add_assoc_str(return_value, "content_type", call->content_type != NULL ? zend_string_copy(call->content_type) : zend_empty_string);
    add_assoc_str(return_value, "grpc_encoding", call->grpc_encoding != NULL ? zend_string_copy(call->grpc_encoding) : zend_empty_string);
    add_assoc_bool(return_value, "compressed_response_seen", call->compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", call->response_message_too_large);
    add_assoc_bool(return_value, "malformed_response_frame", call->malformed_response_frame);
    add_assoc_bool(return_value, "metadata_too_large", call->metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", call->invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", call->unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", call->max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) call->max_receive_message_bytes);
    add_assoc_bool(return_value, "timed_out", call->timed_out);
    add_assoc_bool(return_value, "cancelled", state->cancelled);
    add_assoc_long(return_value, "connection_last_error", state->call.connection != NULL ? state->call.connection->last_error : 0);
    add_assoc_long(return_value, "connection_last_io_errno", state->call.connection != NULL ? state->call.connection->last_io_errno : call->last_io_errno);
    add_assoc_long(return_value, "connection_last_ssl_error", state->call.connection != NULL ? state->call.connection->last_ssl_error : call->last_ssl_error);
    add_assoc_long(return_value, "connection_tls_verify_result", state->call.connection != NULL ? (zend_long) state->call.connection->tls_verify_result : 0);
    add_assoc_string(return_value, "connection_last_error_detail", state->call.connection != NULL ? state->call.connection->last_error_detail : call->last_io_error_detail);
    add_assoc_string(return_value, "connection_negotiated_protocol", state->call.connection != NULL ? state->call.connection->negotiated_protocol : "");
    grpc_protocol_add_metadata_map_to_return(return_value, "initial_metadata", call, false);
    grpc_protocol_add_metadata_map_to_return(return_value, "trailing_metadata", call, true);
    zend_string_release(status_result.details);
}
#endif

static void server_streaming_call_fill_status_result(grpc_lite_streaming_next_result *result, server_streaming_call_state *state)
{
    grpc_lite_status_result status_result;

    resolve_grpc_call_status(&state->call, state->cancelled, &status_result);
    result->done = true;
    result->status.code = status_result.code;
    result->status.details = zend_string_copy(status_result.details);
    grpc_protocol_copy_metadata_map(&result->initial_metadata, &state->call, false);
    grpc_protocol_copy_metadata_map(&result->trailing_metadata, &state->call, true);
    zend_string_release(status_result.details);
}

static int server_streaming_call_next_resource_core(zval *server_streaming_resource_zv,
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    zval *diagnostic_result,
#endif
    grpc_lite_streaming_next_result *typed_result)
{
    server_streaming_call_state *state;
    grpc_call *call;
    uint64_t recv_loop_started_us;

    state = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(server_streaming_resource_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (state == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    call = &state->call;

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    if (diagnostic_result != NULL) {
        array_init(diagnostic_result);
    }
#endif

    recv_loop_started_us = monotonic_us();
    while (call->response_queue_head == NULL && !call->stream_closed && !state->completed && !call->response_message_too_large && !call->compressed_response_seen && !call->malformed_response_frame && !call->invalid_content_type && !call->unsupported_response_encoding && !call->metadata_too_large && !call->response_queue_limit_exceeded) {
        int rv;
        ssize_t nread;
        if (call->deadline_abs_us > 0 && monotonic_us() >= call->deadline_abs_us) {
            call->timed_out = true;
            cancel_active_server_streaming_call_state(state, NGHTTP2_CANCEL);
            state->completed = true;
            break;
        }
        rv = send_pending_h2_frames(state->call.connection, call);
        if (rv != 0) {
            mark_connection_dead(state->call.connection, rv);
            if (call->timed_out) {
                state->completed = true;
                break;
            }
            state->completed = true;
            break;
        }
        nread = connection_recv(state->call.connection, (uint8_t *) state->recv_buf, state->recv_buf_len, call->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && call->deadline_abs_us > 0;
            if (nread < 0) {
                call->last_io_errno = errno;
                call->last_ssl_error = state->call.connection->last_ssl_error;
                snprintf(call->last_io_error_detail, sizeof(call->last_io_error_detail), "%s", state->call.connection->last_error_detail);
            }
            if (socket_timeout) {
                call->timed_out = true;
                cancel_active_server_streaming_call_state(state, NGHTTP2_CANCEL);
            } else {
                mark_connection_dead(state->call.connection, nread == 0 ? 0 : errno);
            }
            state->completed = true;
            break;
        }
        call->bytes_received += (size_t) nread;
        state->call.connection->current_read_call = call;
        rv = nghttp2_session_mem_recv(state->call.connection->session, (const uint8_t *) state->recv_buf, (size_t) nread);
        state->call.connection->current_read_call = NULL;
        if (rv < 0) {
            mark_connection_dead(state->call.connection, rv);
            state->completed = true;
            break;
        }
        if (nghttp2_session_want_write(state->call.connection->session)) {
            rv = send_pending_h2_frames(state->call.connection, call);
            if (rv != 0) {
                mark_connection_dead(state->call.connection, rv);
                state->completed = true;
                break;
            }
        }
    }
    state->recv_loop_us += monotonic_us() - recv_loop_started_us;

    if (call->response_message_too_large || call->compressed_response_seen || call->malformed_response_frame || call->invalid_content_type || call->unsupported_response_encoding || call->metadata_too_large || call->response_queue_limit_exceeded) {
        server_streaming_call_terminate_with_cancel(state);
        if (!connection_usable(state->call.connection)) {
            detach_persistent_connection_by_ptr(state->call.connection);
        }
    }

    if (call->response_queue_head != NULL) {
        queued_payload *entry = call->response_queue_head;
        call->response_queue_head = entry->next;
        if (call->response_queue_head == NULL) {
            call->response_queue_tail = NULL;
        }
        call->response_queue_count--;
        call->response_queue_bytes -= ZSTR_LEN(entry->payload);
        state->delivered_messages++;
        state->delivered_payload_bytes += ZSTR_LEN(entry->payload);
        if (typed_result != NULL) {
            typed_result->done = false;
            typed_result->payload = entry->payload;
            grpc_protocol_copy_metadata_map(&typed_result->initial_metadata, call, false);
        }
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
        if (diagnostic_result != NULL) {
            add_assoc_bool(diagnostic_result, "done", false);
            add_assoc_str(diagnostic_result, "payload", entry->payload);
            grpc_protocol_add_metadata_map_to_return(diagnostic_result, "initial_metadata", call, false);
        }
#endif
        efree(entry);
        return SUCCESS;
    }

    if (!call->response_message_too_large && !call->compressed_response_seen && (call->response_header_len != 0 || call->response_payload != NULL || call->response_payload_offset != 0)) {
        call->malformed_response_frame = true;
    }
    state->completed = true;
    if (typed_result != NULL) {
        server_streaming_call_fill_status_result(typed_result, state);
    }
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    if (diagnostic_result != NULL) {
        server_streaming_call_add_status(diagnostic_result, state);
    }
#endif
    clear_connection_server_streaming_call_state_owner(state);
    return SUCCESS;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static int server_streaming_call_next_resource_diagnostic(zval *server_streaming_resource_zv, zval *return_value)
{
    return server_streaming_call_next_resource_core(server_streaming_resource_zv, return_value, NULL);
}
#endif

static void grpc_lite_streaming_next_result_dtor(grpc_lite_streaming_next_result *result)
{
    if (result->payload != NULL) {
        zend_string_release(result->payload);
    }
    if (result->status.details != NULL) {
        zend_string_release(result->status.details);
    }
    zval_ptr_dtor(&result->initial_metadata);
    zval_ptr_dtor(&result->trailing_metadata);
}

static int server_streaming_call_next_resource(zval *server_streaming_resource_zv, grpc_lite_streaming_next_result *result)
{
    memset(result, 0, sizeof(*result));
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    return server_streaming_call_next_resource_core(server_streaming_resource_zv, NULL, result);
#else
    return server_streaming_call_next_resource_core(server_streaming_resource_zv, result);
#endif
}

static int server_streaming_call_cancel_resource(zval *server_streaming_resource_zv)
{
    server_streaming_call_state *state;

    state = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(server_streaming_resource_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (state == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    if (!state->completed && connection_owned_by_server_streaming_call_state(state->call.connection, state) && connection_usable(state->call.connection)) {
        state->cancelled = true;
        state->call.grpc_status = GRPC_STATUS_CANCELLED;
        server_streaming_call_terminate_with_cancel(state);
        clear_connection_server_streaming_call_state_owner(state);
    }

    return SUCCESS;
}
