#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "grpc_internal.h"

ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)

static int le_h2_stream;

#include "grpc_transport.c"

static int perform_h2_channel_unary(h2_channel *channel, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, zend_long max_receive_message_length, bool channel_reused, bool persistent_reused, zval *return_value)
{
    grpc_call client;
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
    if (!channel_usable(channel)) {
        zend_throw_exception(NULL, "invalid grpc_lite channel", 0);
        return FAILURE;
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "HTTP/2 channel already has an active stream", 0);
        return FAILURE;
    }
    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }

    memset(&client, 0, sizeof(client));
    client.fd = channel->fd;
    client.channel = channel;
    client.grpc_status = -1;
    client.http_status = -1;
    client.max_response_messages = 1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    client.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    total_started = monotonic_us();
    client.deadline_abs_us = timeout_us > 0 ? total_started + (uint64_t) timeout_us : 0;
    if (set_socket_timeout_us(channel->fd, timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    setup_started = monotonic_us();
    // cppcheck-suppress autoVariables
    nghttp2_session_set_user_data(channel->session, &client);
    channel->busy = true;
    // cppcheck-suppress autoVariables
    channel->active_call_owner = &client;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        clear_channel_call_owner(channel, &client);
        cleanup_grpc_call(&client);
        return FAILURE;
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, channel->tls ? "https" : "http", channel->tls ? sizeof("https") - 1 : sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, channel->authority, strlen(channel->authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    setup_us = monotonic_us() - setup_started;

    submit_started = monotonic_us();
    client.stream_id = nghttp2_submit_request(channel->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    submit_us = monotonic_us() - submit_started;
    if (client.stream_id < 0) {
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    initial_send_started = monotonic_us();
    rv = nghttp2_session_send(channel->session);
    initial_send_us = monotonic_us() - initial_send_started;
    if (rv != 0) {
        mark_channel_dead(channel, rv);
        if (client.timed_out) {
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            recv_loop_us = 0;
            goto build_unary_result;
        }
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        return FAILURE;
    }

    recv_loop_started = monotonic_us();
    while (!client.stream_closed) {
        ssize_t nread = channel_recv(channel, (uint8_t *) recv_buf, sizeof(recv_buf), client.deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && client.deadline_abs_us > 0;
            if (nread < 0) {
                client.last_io_errno = errno;
                client.last_ssl_error = channel->last_ssl_error;
                snprintf(client.last_io_error_detail, sizeof(client.last_io_error_detail), "%s", channel->last_error_detail);
            }
            if (socket_timeout) {
                client.timed_out = true;
            }
            if (!client.stream_closed) {
                mark_channel_dead(channel, nread == 0 ? 0 : errno);
            }
            break;
        }
        client.bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(channel->session, (const uint8_t *) recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_channel_dead(channel, rv);
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&client);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            return FAILURE;
        }
        rv = nghttp2_session_send(channel->session);
        if (rv != 0) {
            mark_channel_dead(channel, rv);
            if (client.timed_out) {
                break;
            }
            if (client.stream_closed) {
                break;
            }
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&client);
            zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
            return FAILURE;
        }
        client.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;
build_unary_result:
    clear_channel_call_owner(channel, &client);

    array_init(return_value);
    smart_str_0(&client.body);
    add_assoc_str(return_value, "body", client.body.s ? zend_string_copy(client.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_str(return_value, "grpc_message", client.grpc_message != NULL ? zend_string_copy(client.grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
    add_assoc_bool(return_value, "invalid_grpc_status", client.invalid_grpc_status);
    add_assoc_bool(return_value, "compressed_response_seen", client.compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", client.response_message_too_large);
    add_assoc_bool(return_value, "metadata_too_large", client.metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", client.invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", client.unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", client.max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) client.max_receive_message_bytes);
    add_assoc_long(return_value, "body_bytes", client.body.s ? ZSTR_LEN(client.body.s) : 0);
    add_assoc_long(return_value, "request_offset", client.request_offset);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "data_read_calls", client.data_read_calls);
    add_assoc_long(return_value, "data_recv_calls", client.data_recv_calls);
    add_assoc_long(return_value, "last_session_error", client.last_session_error);
    add_assoc_long(return_value, "last_sent_frame_type", client.last_sent_frame_type);
    add_assoc_long(return_value, "last_recv_frame_type", client.last_recv_frame_type);
    add_assoc_long(return_value, "last_sent_frame_flags", client.last_sent_frame_flags);
    add_assoc_long(return_value, "last_recv_frame_flags", client.last_recv_frame_flags);
    add_assoc_long(return_value, "last_not_sent_frame_type", client.last_not_sent_frame_type);
    add_assoc_long(return_value, "last_not_sent_error", client.last_not_sent_error);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_long(return_value, "not_sent_frames", client.not_sent_frames);
    add_assoc_long(return_value, "last_io_errno", client.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", client.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", client.last_io_error_detail);
    add_assoc_long(return_value, "total_us", (zend_long) (monotonic_us() - total_started));
    add_assoc_long(return_value, "connect_us", 0);
    add_assoc_long(return_value, "setup_us", (zend_long) setup_us);
    add_assoc_long(return_value, "submit_us", (zend_long) submit_us);
    add_assoc_long(return_value, "initial_send_us", (zend_long) initial_send_us);
    add_assoc_long(return_value, "recv_loop_us", (zend_long) recv_loop_us);
    add_assoc_long(return_value, "cleanup_us", 0);
    add_assoc_bool(return_value, "timed_out", client.timed_out);
    add_assoc_bool(return_value, "channel_reused", channel_reused);
    add_assoc_bool(return_value, "persistent_reused", persistent_reused);
    add_assoc_bool(return_value, "channel_dead", channel->dead);
    add_assoc_bool(return_value, "channel_draining", channel->draining);
    add_assoc_long(return_value, "channel_last_error", channel->last_error);
    add_assoc_long(return_value, "channel_last_io_errno", channel->last_io_errno);
    add_assoc_long(return_value, "channel_last_ssl_error", channel->last_ssl_error);
    add_assoc_long(return_value, "channel_tls_verify_result", (zend_long) channel->tls_verify_result);
    add_assoc_string(return_value, "channel_last_error_detail", channel->last_error_detail);
    add_assoc_string(return_value, "channel_negotiated_protocol", channel->negotiated_protocol);
    add_assoc_long(return_value, "channel_last_goaway_error_code", channel->last_goaway_error_code);
    add_assoc_long(return_value, "channel_last_goaway_stream_id", channel->last_goaway_stream_id);
    add_metadata_map_to_return(return_value, "initial_metadata", &client, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", &client, true);
    free_request_headers(&request_headers);
    cleanup_grpc_call(&client);
    return SUCCESS;
}



PHP_FUNCTION(grpc_lite_unary)
{
    char *key = NULL;
    size_t key_len = 0;
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    zend_long timeout_us = 0;
    bool use_tls = false;
    char *root_certs = NULL;
    size_t root_certs_len = 0;
    char *cert_chain = NULL;
    size_t cert_chain_len = 0;
    char *private_key = NULL;
    size_t private_key_len = 0;
    zend_long max_receive_message_length = 0;
    char *authority = NULL;
    size_t authority_len = 0;
    char *tls_verify_name = NULL;
    size_t tls_verify_name_len = 0;
    h2_channel *channel;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;

    ZEND_PARSE_PARAMETERS_START(5, 14)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_LONG(timeout_us)
        Z_PARAM_BOOL(use_tls)
        Z_PARAM_STRING_OR_NULL(root_certs, root_certs_len)
        Z_PARAM_STRING_OR_NULL(cert_chain, cert_chain_len)
        Z_PARAM_STRING_OR_NULL(private_key, private_key_len)
        Z_PARAM_LONG(max_receive_message_length)
        Z_PARAM_STRING_OR_NULL(authority, authority_len)
        Z_PARAM_STRING_OR_NULL(tls_verify_name, tls_verify_name_len)
    ZEND_PARSE_PARAMETERS_END();

    error_message = validate_channel_inputs(key, key_len, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }

    if (!PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        zend_throw_exception(NULL, "persistent channel cache is not initialized", 0);
        RETURN_THROWS();
    }

    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    channel = get_persistent_channel(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }

    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        RETURN_THROWS();
    }

    if (perform_h2_channel_unary(channel, path, path_len, request, request_len, headers_zv, remaining_timeout_us, max_receive_message_length, true, persistent_reused, return_value) != SUCCESS) {
        if (channel != NULL && !channel_usable(channel)) {
            remove_unusable_persistent_channel(key, key_len, channel);
        }
        RETURN_THROWS();
    }

    if (!channel_usable(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
    }
}

PHP_FUNCTION(grpc_lite_stream_open)
{
    char *key = NULL;
    size_t key_len = 0;
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    zend_long timeout_us = 0;
    bool use_tls = false;
    char *root_certs = NULL;
    size_t root_certs_len = 0;
    char *cert_chain = NULL;
    size_t cert_chain_len = 0;
    char *private_key = NULL;
    size_t private_key_len = 0;
    zend_long max_receive_message_length = 0;
    char *authority = NULL;
    size_t authority_len = 0;
    char *tls_verify_name = NULL;
    size_t tls_verify_name_len = 0;
    h2_channel *channel;
    h2_stream *stream;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;
    int rv;

    ZEND_PARSE_PARAMETERS_START(5, 14)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_LONG(timeout_us)
        Z_PARAM_BOOL(use_tls)
        Z_PARAM_STRING_OR_NULL(root_certs, root_certs_len)
        Z_PARAM_STRING_OR_NULL(cert_chain, cert_chain_len)
        Z_PARAM_STRING_OR_NULL(private_key, private_key_len)
        Z_PARAM_LONG(max_receive_message_length)
        Z_PARAM_STRING_OR_NULL(authority, authority_len)
        Z_PARAM_STRING_OR_NULL(tls_verify_name, tls_verify_name_len)
    ZEND_PARSE_PARAMETERS_END();

    error_message = validate_channel_inputs(key, key_len, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }

    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        RETURN_THROWS();
    }
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    channel = get_persistent_channel(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "HTTP/2 channel already has an active stream", 0);
        RETURN_THROWS();
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        RETURN_THROWS();
    }
    if (set_socket_timeout_us(channel->fd, remaining_timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        discard_persistent_channel(key, key_len, channel);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        RETURN_THROWS();
    }

    stream = ecalloc(1, sizeof(h2_stream));
    stream->channel = channel;
    stream->request = zend_string_init(request, request_len, 0);
    stream->recv_buf_len = 65536;
    stream->recv_buf = emalloc(stream->recv_buf_len);

    memset(&stream->client, 0, sizeof(stream->client));
    stream->client.fd = channel->fd;
    stream->client.channel = channel;
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

    nghttp2_session_set_user_data(channel->session, &stream->client);
    channel->busy = true;
    channel->active_stream_owner = stream;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        destroy_h2_stream(stream);
        RETURN_THROWS();
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, channel->tls ? "https" : "http", channel->tls ? sizeof("https") - 1 : sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, channel->authority, strlen(channel->authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        RETURN_THROWS();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    stream->client.stream_id = nghttp2_submit_request(channel->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    if (stream->client.stream_id < 0) {
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        RETURN_THROWS();
    }

    rv = nghttp2_session_send(channel->session);
    if (rv != 0) {
        bool stream_timed_out = stream->client.timed_out;
        mark_channel_dead(channel, rv);
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        discard_persistent_channel(key, key_len, channel);
        if (stream_timed_out) {
            zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
            RETURN_THROWS();
        }
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        RETURN_THROWS();
    }

    free_request_headers(&request_headers);
    RETURN_RES(zend_register_resource(stream, le_h2_stream));
}

static void add_stream_status(zval *return_value, h2_stream *stream)
{
    grpc_call *client = &stream->client;
    add_assoc_bool(return_value, "done", true);
    add_assoc_long(return_value, "grpc_status", client->grpc_status);
    add_assoc_str(return_value, "grpc_message", client->grpc_message != NULL ? zend_string_copy(client->grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", client->http_status);
    add_assoc_long(return_value, "stream_error_code", client->stream_error_code);
    add_assoc_bool(return_value, "invalid_grpc_status", client->invalid_grpc_status);
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
    add_assoc_bool(return_value, "channel_dead", stream->channel != NULL ? stream->channel->dead : false);
    add_assoc_bool(return_value, "channel_draining", stream->channel != NULL ? stream->channel->draining : false);
    add_assoc_long(return_value, "channel_last_error", stream->channel != NULL ? stream->channel->last_error : 0);
    add_assoc_long(return_value, "channel_last_io_errno", stream->channel != NULL ? stream->channel->last_io_errno : client->last_io_errno);
    add_assoc_long(return_value, "channel_last_ssl_error", stream->channel != NULL ? stream->channel->last_ssl_error : client->last_ssl_error);
    add_assoc_long(return_value, "channel_tls_verify_result", stream->channel != NULL ? (zend_long) stream->channel->tls_verify_result : 0);
    add_assoc_string(return_value, "channel_last_error_detail", stream->channel != NULL ? stream->channel->last_error_detail : client->last_io_error_detail);
    add_assoc_string(return_value, "channel_negotiated_protocol", stream->channel != NULL ? stream->channel->negotiated_protocol : "");
    add_metadata_map_to_return(return_value, "initial_metadata", client, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", client, true);
}

PHP_FUNCTION(grpc_lite_stream_next)
{
    zval *stream_zv = NULL;
    h2_stream *stream;
    grpc_call *client;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (h2_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_stream", le_h2_stream);
    if (stream == NULL) {
        RETURN_THROWS();
    }
    client = &stream->client;

    array_init(return_value);

    while (client->response_queue_head == NULL && !client->stream_closed && !stream->completed && !client->response_message_too_large && !client->compressed_response_seen && !client->malformed_response_frame && !client->invalid_content_type && !client->unsupported_response_encoding && !client->metadata_too_large) {
        int rv;
        ssize_t nread;
        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            cancel_active_stream(stream, NGHTTP2_CANCEL);
            stream->completed = true;
            break;
        }
        rv = nghttp2_session_send(stream->channel->session);
        if (rv != 0) {
            mark_channel_dead(stream->channel, rv);
            if (client->timed_out) {
                stream->completed = true;
                break;
            }
            stream->completed = true;
            break;
        }
        nread = channel_recv(stream->channel, (uint8_t *) stream->recv_buf, stream->recv_buf_len, client->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && client->deadline_abs_us > 0;
            if (nread < 0) {
                client->last_io_errno = errno;
                client->last_ssl_error = stream->channel->last_ssl_error;
                snprintf(client->last_io_error_detail, sizeof(client->last_io_error_detail), "%s", stream->channel->last_error_detail);
            }
            if (socket_timeout) {
                client->timed_out = true;
                cancel_active_stream(stream, NGHTTP2_CANCEL);
            } else {
                mark_channel_dead(stream->channel, nread == 0 ? 0 : errno);
            }
            stream->completed = true;
            break;
        }
        client->bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(stream->channel->session, (const uint8_t *) stream->recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_channel_dead(stream->channel, rv);
            stream->completed = true;
            break;
        }
    }

    if (client->response_message_too_large || client->compressed_response_seen || client->malformed_response_frame || client->invalid_content_type || client->unsupported_response_encoding || client->metadata_too_large) {
        if (channel_owned_by_stream(stream->channel, stream) && channel_usable(stream->channel)) {
            int rv = nghttp2_session_send(stream->channel->session);
            if (rv != 0) {
                mark_channel_dead(stream->channel, rv);
            }
        }
        free_queued_response_payloads(client);
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
        efree(entry);
        return;
    }

    if (!client->response_message_too_large && !client->compressed_response_seen && (client->response_header_len != 0 || client->response_payload != NULL || client->response_payload_offset != 0)) {
        client->malformed_response_frame = true;
    }
    stream->completed = true;
    add_stream_status(return_value, stream);
    clear_channel_stream_owner(stream);
}

PHP_FUNCTION(grpc_lite_stream_cancel)
{
    zval *stream_zv = NULL;
    h2_stream *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (h2_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_stream", le_h2_stream);
    if (stream == NULL) {
        RETURN_THROWS();
    }
    if (stream != NULL && !stream->completed && channel_owned_by_stream(stream->channel, stream) && channel_usable(stream->channel)) {
        int rv;
        stream->cancelled = true;
        stream->completed = true;
        stream->client.grpc_status = 1;
        rv = nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        if (rv != 0) {
            mark_channel_dead(stream->channel, rv);
            clear_channel_stream_owner(stream);
            RETURN_FALSE;
        } else {
            rv = nghttp2_session_send(stream->channel->session);
            if (rv != 0) {
                mark_channel_dead(stream->channel, rv);
                clear_channel_stream_owner(stream);
                RETURN_FALSE;
            }
        }
        clear_channel_stream_owner(stream);
    }

    RETURN_TRUE;
}

PHP_FUNCTION(grpc_lite_channel_close)
{
    char *key = NULL;
    size_t key_len = 0;
    h2_channel *channel;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(key, key_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        RETURN_FALSE;
    }

    channel = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_channels), key, key_len);
    if (channel == NULL) {
        RETURN_FALSE;
    }

    discard_persistent_channel(key, key_len, channel);
    RETURN_TRUE;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
#include "grpc_bench.c"
#endif

PHP_GINIT_FUNCTION(grpc_lite)
{
#if defined(COMPILE_DL_GRPC) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_init(&grpc_lite_globals->persistent_channels, 8, NULL, NULL, 1);
    grpc_lite_globals->persistent_channels_initialized = true;
}

PHP_GSHUTDOWN_FUNCTION(grpc_lite)
{
    h2_channel *channel;

    if (!grpc_lite_globals->persistent_channels_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&grpc_lite_globals->persistent_channels, channel) {
        destroy_h2_channel(channel);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&grpc_lite_globals->persistent_channels);
    grpc_lite_globals->persistent_channels_initialized = false;
}

PHP_MINIT_FUNCTION(grpc_lite)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    le_h2_stream = zend_register_list_destructors_ex(h2_stream_dtor, NULL, "grpc_lite_stream", module_number);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(grpc_lite)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "grpc_lite bridge", "enabled");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_unary, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_tls, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, max_receive_message_length, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, authority, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls_verify_name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_grpc_lite_stream_open, 0, 0, 5)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_tls, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, max_receive_message_length, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, authority, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls_verify_name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_stream_next, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_stream_cancel, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_channel_close, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry grpc_lite_functions[] = {
    PHP_FE(grpc_lite_unary, arginfo_grpc_lite_unary)
    PHP_FE(grpc_lite_stream_open, arginfo_grpc_lite_stream_open)
    PHP_FE(grpc_lite_stream_next, arginfo_grpc_lite_stream_next)
    PHP_FE(grpc_lite_stream_cancel, arginfo_grpc_lite_stream_cancel)
    PHP_FE(grpc_lite_channel_close, arginfo_grpc_lite_channel_close)
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    PHP_FE(grpc_lite_multiplex_unary, arginfo_grpc_lite_multiplex_unary)
    PHP_FE(grpc_lite_bench_unary_batch, arginfo_grpc_lite_bench_unary_batch)
#endif
    PHP_FE_END
};

zend_module_entry grpc_module_entry = {
    STANDARD_MODULE_HEADER,
    "grpc",
    grpc_lite_functions,
    PHP_MINIT(grpc_lite),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(grpc_lite),
    "0.1.0",
    ZEND_MODULE_GLOBALS(grpc_lite),
    PHP_GINIT(grpc_lite),
    PHP_GSHUTDOWN(grpc_lite),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_GRPC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(grpc)
#endif
