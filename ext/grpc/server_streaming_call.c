/* Server streaming gRPC client call execution over an HTTP/2 connection. Included by main.c. */

#include "internal.h"

static void server_streaming_call_terminate_with_cancel(server_streaming_call_state *stream)
{
    if (stream == NULL) {
        return;
    }
    free_queued_response_payloads(&stream->client);
    if (connection_owned_by_server_streaming_call_state(stream->connection, stream) && connection_usable(stream->connection) && stream->client.stream_id > 0) {
        int rv = nghttp2_submit_rst_stream(stream->connection->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        if (rv == 0) {
            rv = nghttp2_session_send(stream->connection->session);
        }
        if (rv != 0) {
            mark_connection_dead(stream->connection, rv);
            detach_persistent_connection_by_ptr(stream->connection);
        }
    }
    stream->completed = true;
}
static int server_streaming_call_open_resource(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, zend_long max_receive_message_length, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, zval *return_value)
{
    h2_connection *connection;
    server_streaming_call_state *stream;
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
        zend_throw_exception(NULL, "HTTP/2 connection already has an active stream", 0);
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

    stream = ecalloc(1, sizeof(server_streaming_call_state));
    stream->connection = connection;
    stream->request = zend_string_init(request, request_len, 0);
    stream->recv_buf_len = 65536;
    stream->recv_buf = emalloc(stream->recv_buf_len);

    memset(&stream->client, 0, sizeof(stream->client));
    stream->client.fd = connection->fd;
    stream->client.connection = connection;
    stream->client.grpc_status = -1;
    stream->client.http_status = -1;
    stream->client.request = (const uint8_t *) ZSTR_VAL(stream->request);
    stream->client.request_len = ZSTR_LEN(stream->request);
    stream->client.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    stream->client.deadline_abs_us = deadline_abs_us > 0 ? deadline_abs_us : 0;
    stream->client.decode_response_incrementally = true;
    stream->client.direct_response_payload = true;
    stream->client.queue_response_payloads = true;
    set_grpc_header(&stream->client, stream->client.request_len);

    nghttp2_session_set_user_data(connection->session, &stream->client);
    connection->busy = true;
    connection->active_server_streaming_call_owner = stream;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        destroy_server_streaming_call_state(stream);
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
        destroy_server_streaming_call_state(stream);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    stream->client.stream_id = nghttp2_submit_request(connection->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    if (stream->client.stream_id < 0) {
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(stream);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    rv = nghttp2_session_send(connection->session);
    if (rv != 0) {
        bool stream_timed_out = stream->client.timed_out;
        mark_connection_dead(connection, rv);
        free_request_headers(&request_headers);
        destroy_server_streaming_call_state(stream);
        if (stream_timed_out) {
            zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
            return FAILURE;
        }
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        return FAILURE;
    }

    free_request_headers(&request_headers);
    ZVAL_RES(return_value, zend_register_resource(stream, le_server_streaming_call_state));
    return SUCCESS;
}

static void server_streaming_call_add_status(zval *return_value, server_streaming_call_state *stream)
{
    grpc_call *client = &stream->client;
    add_assoc_bool(return_value, "done", true);
    add_assoc_long(return_value, "grpc_status", client->grpc_status);
    add_assoc_str(return_value, "grpc_message", client->grpc_message != NULL ? zend_string_copy(client->grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", client->http_status);
    add_assoc_long(return_value, "stream_error_code", client->stream_error_code);
    add_assoc_bool(return_value, "stream_reset_seen", client->stream_reset_seen);
    add_assoc_bool(return_value, "invalid_grpc_status", client->invalid_grpc_status);
    add_assoc_str(return_value, "content_type", client->content_type != NULL ? zend_string_copy(client->content_type) : zend_empty_string);
    add_assoc_str(return_value, "grpc_encoding", client->grpc_encoding != NULL ? zend_string_copy(client->grpc_encoding) : zend_empty_string);
    add_assoc_bool(return_value, "compressed_response_seen", client->compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", client->response_message_too_large);
    add_assoc_bool(return_value, "malformed_response_frame", client->malformed_response_frame);
    add_assoc_bool(return_value, "metadata_too_large", client->metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", client->invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", client->unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", client->max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) client->max_receive_message_bytes);
    add_assoc_bool(return_value, "timed_out", client->timed_out);
    add_assoc_bool(return_value, "cancelled", stream->cancelled);
    add_assoc_long(return_value, "body_bytes", 0);
    add_assoc_long(return_value, "bytes_sent", client->bytes_sent);
    add_assoc_long(return_value, "bytes_received", client->bytes_received);
    add_assoc_bool(return_value, "channel_dead", stream->connection != NULL ? stream->connection->dead : false);
    add_assoc_bool(return_value, "channel_draining", stream->connection != NULL ? stream->connection->draining : false);
    add_assoc_long(return_value, "channel_last_error", stream->connection != NULL ? stream->connection->last_error : 0);
    add_assoc_long(return_value, "channel_last_io_errno", stream->connection != NULL ? stream->connection->last_io_errno : client->last_io_errno);
    add_assoc_long(return_value, "channel_last_ssl_error", stream->connection != NULL ? stream->connection->last_ssl_error : client->last_ssl_error);
    add_assoc_long(return_value, "channel_tls_verify_result", stream->connection != NULL ? (zend_long) stream->connection->tls_verify_result : 0);
    add_assoc_string(return_value, "channel_last_error_detail", stream->connection != NULL ? stream->connection->last_error_detail : client->last_io_error_detail);
    add_assoc_string(return_value, "channel_negotiated_protocol", stream->connection != NULL ? stream->connection->negotiated_protocol : "");
    add_metadata_map_to_return(return_value, "initial_metadata", client, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", client, true);
}

static int server_streaming_call_next_resource(zval *stream_zv, zval *return_value)
{
    server_streaming_call_state *stream;
    grpc_call *client;

    stream = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (stream == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    client = &stream->client;

    array_init(return_value);

    while (client->response_queue_head == NULL && !client->stream_closed && !stream->completed && !client->response_message_too_large && !client->compressed_response_seen && !client->malformed_response_frame && !client->invalid_content_type && !client->unsupported_response_encoding && !client->metadata_too_large) {
        int rv;
        ssize_t nread;
        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            cancel_active_server_streaming_call_state(stream, NGHTTP2_CANCEL);
            stream->completed = true;
            break;
        }
        rv = nghttp2_session_send(stream->connection->session);
        if (rv != 0) {
            mark_connection_dead(stream->connection, rv);
            if (client->timed_out) {
                stream->completed = true;
                break;
            }
            stream->completed = true;
            break;
        }
        nread = connection_recv(stream->connection, (uint8_t *) stream->recv_buf, stream->recv_buf_len, client->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && client->deadline_abs_us > 0;
            if (nread < 0) {
                client->last_io_errno = errno;
                client->last_ssl_error = stream->connection->last_ssl_error;
                snprintf(client->last_io_error_detail, sizeof(client->last_io_error_detail), "%s", stream->connection->last_error_detail);
            }
            if (socket_timeout) {
                client->timed_out = true;
                cancel_active_server_streaming_call_state(stream, NGHTTP2_CANCEL);
            } else {
                mark_connection_dead(stream->connection, nread == 0 ? 0 : errno);
            }
            stream->completed = true;
            break;
        }
        client->bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(stream->connection->session, (const uint8_t *) stream->recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_connection_dead(stream->connection, rv);
            stream->completed = true;
            break;
        }
    }

    if (client->response_message_too_large || client->compressed_response_seen || client->malformed_response_frame || client->invalid_content_type || client->unsupported_response_encoding || client->metadata_too_large) {
        server_streaming_call_terminate_with_cancel(stream);
        detach_persistent_connection_by_ptr(stream->connection);
    }

    if (client->response_queue_head != NULL) {
        queued_payload *entry = client->response_queue_head;
        client->response_queue_head = entry->next;
        if (client->response_queue_head == NULL) {
            client->response_queue_tail = NULL;
        }
        client->response_queue_count--;
        client->response_queue_bytes -= ZSTR_LEN(entry->payload);
        add_assoc_bool(return_value, "done", false);
        add_assoc_str(return_value, "payload", entry->payload);
        add_metadata_map_to_return(return_value, "initial_metadata", client, false);
        efree(entry);
        return SUCCESS;
    }

    if (!client->response_message_too_large && !client->compressed_response_seen && (client->response_header_len != 0 || client->response_payload != NULL || client->response_payload_offset != 0)) {
        client->malformed_response_frame = true;
    }
    stream->completed = true;
    server_streaming_call_add_status(return_value, stream);
    clear_connection_server_streaming_call_state_owner(stream);
    return SUCCESS;
}

static int server_streaming_call_cancel_resource(zval *stream_zv)
{
    server_streaming_call_state *stream;

    stream = (server_streaming_call_state *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_server_streaming_call_state", le_server_streaming_call_state);
    if (stream == NULL) {
        zend_throw_exception(NULL, "invalid grpc_lite_server_streaming_call_state resource", 0);
        return FAILURE;
    }
    if (stream != NULL && !stream->completed && connection_owned_by_server_streaming_call_state(stream->connection, stream) && connection_usable(stream->connection)) {
        stream->cancelled = true;
        stream->client.grpc_status = 1;
        server_streaming_call_terminate_with_cancel(stream);
        clear_connection_server_streaming_call_state_owner(stream);
    }

    return SUCCESS;
}
