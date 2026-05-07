/* Server streaming gRPC client call execution over an HTTP/2 connection. Included by main.c. */

#include "internal.h"

static void server_streaming_call_terminate_with_cancel(server_streaming_call_state *state)
{
    if (state == NULL) {
        return;
    }
    free_queued_response_payloads(&state->call);
    if (connection_owned_by_server_streaming_call_state(state->connection, state) && connection_usable(state->connection) && state->call.stream_id > 0) {
        int rv = nghttp2_submit_rst_stream(state->connection->session, NGHTTP2_FLAG_NONE, state->call.stream_id, NGHTTP2_CANCEL);
        if (rv == 0) {
            rv = nghttp2_session_send(state->connection->session);
        }
        if (rv != 0) {
            mark_connection_dead(state->connection, rv);
            detach_persistent_connection_by_ptr(state->connection);
        }
    }
    state->completed = true;
}
static int server_streaming_call_open_resource(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, zend_long max_receive_message_length, size_t max_response_metadata_bytes, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, zval *return_value)
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
    connection = get_persistent_connection(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (connection == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent connection", 0);
        return FAILURE;
    }
    if (connection->busy) {
        zend_throw_exception(NULL, "HTTP/2 connection already has an active HTTP/2 stream", 0);
        return FAILURE;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        return FAILURE;
    }
    if (set_socket_timeout_us(connection->fd, remaining_timeout_us) != 0) {
        mark_connection_dead(connection, errno);
        discard_persistent_connection(key, key_len, connection);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    state = ecalloc(1, sizeof(server_streaming_call_state));
    state->connection = connection;
    state->request = zend_string_init(request, request_len, 0);
    state->recv_buf_len = 65536;
    state->recv_buf = emalloc(state->recv_buf_len);

    memset(&state->call, 0, sizeof(state->call));
    state->call.fd = connection->fd;
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
    set_grpc_header(&state->call, state->call.request_len);

    nghttp2_session_set_user_data(connection->session, &state->call);
    connection->busy = true;
    connection->active_server_streaming_call_owner = state;
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
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    state->call.stream_id = nghttp2_submit_request(connection->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    if (state->call.stream_id < 0) {
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(state);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    rv = nghttp2_session_send(connection->session);
    if (rv != 0) {
        bool stream_timed_out = state->call.timed_out;
        mark_connection_dead(connection, rv);
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

static void server_streaming_call_add_status(zval *return_value, server_streaming_call_state *state)
{
    grpc_call *call = &state->call;
    grpc_lite_status_result status_result;
    resolve_grpc_call_status(call, state->cancelled, &status_result);
    add_assoc_bool(return_value, "done", true);
    add_status_result_to_return(return_value, &status_result);
    add_assoc_long(return_value, "grpc_status", call->grpc_status);
    add_assoc_str(return_value, "grpc_message", call->grpc_message != NULL ? zend_string_copy(call->grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", call->http_status);
    add_assoc_long(return_value, "stream_error_code", call->stream_error_code);
    add_assoc_bool(return_value, "stream_reset_seen", call->stream_reset_seen);
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
    add_assoc_long(return_value, "body_bytes", 0);
    add_assoc_long(return_value, "bytes_sent", call->bytes_sent);
    add_assoc_long(return_value, "bytes_received", call->bytes_received);
    add_assoc_bool(return_value, "connection_dead", state->connection != NULL ? state->connection->dead : false);
    add_assoc_bool(return_value, "connection_draining", state->connection != NULL ? state->connection->draining : false);
    add_assoc_long(return_value, "connection_last_error", state->connection != NULL ? state->connection->last_error : 0);
    add_assoc_long(return_value, "connection_last_io_errno", state->connection != NULL ? state->connection->last_io_errno : call->last_io_errno);
    add_assoc_long(return_value, "connection_last_ssl_error", state->connection != NULL ? state->connection->last_ssl_error : call->last_ssl_error);
    add_assoc_long(return_value, "connection_tls_verify_result", state->connection != NULL ? (zend_long) state->connection->tls_verify_result : 0);
    add_assoc_string(return_value, "connection_last_error_detail", state->connection != NULL ? state->connection->last_error_detail : call->last_io_error_detail);
    add_assoc_string(return_value, "connection_negotiated_protocol", state->connection != NULL ? state->connection->negotiated_protocol : "");
    add_metadata_map_to_return(return_value, "initial_metadata", call, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", call, true);
    zend_string_release(status_result.details);
}

static int server_streaming_call_next_resource(zval *server_streaming_resource_zv, zval *return_value)
{
    server_streaming_call_state *state;
    grpc_call *call;

    state = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(server_streaming_resource_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (state == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    call = &state->call;

    array_init(return_value);

    while (call->response_queue_head == NULL && !call->stream_closed && !state->completed && !call->response_message_too_large && !call->compressed_response_seen && !call->malformed_response_frame && !call->invalid_content_type && !call->unsupported_response_encoding && !call->metadata_too_large) {
        int rv;
        ssize_t nread;
        if (call->deadline_abs_us > 0 && monotonic_us() >= call->deadline_abs_us) {
            call->timed_out = true;
            cancel_active_server_streaming_call_state(state, NGHTTP2_CANCEL);
            state->completed = true;
            break;
        }
        rv = nghttp2_session_send(state->connection->session);
        if (rv != 0) {
            mark_connection_dead(state->connection, rv);
            if (call->timed_out) {
                state->completed = true;
                break;
            }
            state->completed = true;
            break;
        }
        nread = connection_recv(state->connection, (uint8_t *) state->recv_buf, state->recv_buf_len, call->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && call->deadline_abs_us > 0;
            if (nread < 0) {
                call->last_io_errno = errno;
                call->last_ssl_error = state->connection->last_ssl_error;
                snprintf(call->last_io_error_detail, sizeof(call->last_io_error_detail), "%s", state->connection->last_error_detail);
            }
            if (socket_timeout) {
                call->timed_out = true;
                cancel_active_server_streaming_call_state(state, NGHTTP2_CANCEL);
            } else {
                mark_connection_dead(state->connection, nread == 0 ? 0 : errno);
            }
            state->completed = true;
            break;
        }
        call->bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(state->connection->session, (const uint8_t *) state->recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_connection_dead(state->connection, rv);
            state->completed = true;
            break;
        }
        if (nghttp2_session_want_write(state->connection->session)) {
            rv = nghttp2_session_send(state->connection->session);
            if (rv != 0) {
                mark_connection_dead(state->connection, rv);
                state->completed = true;
                break;
            }
        }
    }

    if (call->response_message_too_large || call->compressed_response_seen || call->malformed_response_frame || call->invalid_content_type || call->unsupported_response_encoding || call->metadata_too_large) {
        server_streaming_call_terminate_with_cancel(state);
        if (!connection_usable(state->connection)) {
            detach_persistent_connection_by_ptr(state->connection);
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
        add_assoc_bool(return_value, "done", false);
        add_assoc_str(return_value, "payload", entry->payload);
        add_metadata_map_to_return(return_value, "initial_metadata", call, false);
        efree(entry);
        return SUCCESS;
    }

    if (!call->response_message_too_large && !call->compressed_response_seen && (call->response_header_len != 0 || call->response_payload != NULL || call->response_payload_offset != 0)) {
        call->malformed_response_frame = true;
    }
    state->completed = true;
    server_streaming_call_add_status(return_value, state);
    clear_connection_server_streaming_call_state_owner(state);
    return SUCCESS;
}

static int server_streaming_call_cancel_resource(zval *server_streaming_resource_zv)
{
    server_streaming_call_state *state;

    state = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(server_streaming_resource_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (state == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    if (state != NULL && !state->completed && connection_owned_by_server_streaming_call_state(state->connection, state) && connection_usable(state->connection)) {
        state->cancelled = true;
        state->call.grpc_status = 1;
        server_streaming_call_terminate_with_cancel(state);
        clear_connection_server_streaming_call_state_owner(state);
    }

    return SUCCESS;
}
