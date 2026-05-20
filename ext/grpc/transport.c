#include "internal.h"

/*
 * HTTP/2 transport helpers included by main.c.
 *
 * This file intentionally shares main.c's static scope: it owns connection
 * lifecycle, socket/TLS I/O, nghttp2 callbacks, request header assembly,
 * response frame parsing, and metadata storage helpers. PHP entrypoints and
 * module registration stay in main.c.
 */

#include "transport_core.c"

#define GRPC_LITE_H2_WRITE_COALESCE_CAPACITY 16384
#define GRPC_LITE_PREFLIGHT_DRAIN_CHUNK_SIZE 4096
#define GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES 65536
#define GRPC_LITE_PREFLIGHT_DRAIN_MAX_ITERATIONS 64

static void destroy_h2_connection(h2_connection *connection);
static void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection);
static void detach_persistent_connection_by_ptr(h2_connection *connection);
static bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *state);
static bool connection_owned_by_call(h2_connection *connection, grpc_call *call);
static void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *state);
static void clear_connection_call_owner(h2_connection *connection, grpc_call *call);
static void cancel_active_server_streaming_call_state(server_streaming_call_state *state, uint32_t error_code);
static void destroy_server_streaming_call_state(server_streaming_call_state *state);
static void server_streaming_call_state_dtor(zend_resource *rsrc);
static int configure_callbacks(nghttp2_session_callbacks **callbacks);
static void mark_connection_dead(h2_connection *connection, int error_code);
static void set_connection_error_detail(h2_connection *connection, const char *detail);
static void mark_connection_draining(h2_connection *connection, int32_t last_stream_id, uint32_t error_code);
static bool connection_usable(h2_connection *connection);
static persistent_connection_entry *create_persistent_connection_entry(h2_connection *connection, const char *key, size_t key_len);
static bool connection_entry_matches_key(persistent_connection_entry *entry, const char *key, size_t key_len);
static bool drain_pending_connection_data_for_reuse(h2_connection *connection, uint64_t deadline_abs_us);
static bool preflight_persistent_connection(h2_connection *connection, uint64_t deadline_abs_us);
static void remove_unusable_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
static int set_fd_nonblocking_mode(int fd, bool nonblocking);
static int poll_timeout_ms_for_deadline(uint64_t deadline_abs_us);
static zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us);
static int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us);
static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len);
static int configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len);
static int configure_tls_connection(h2_connection *connection, const char *host, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us);
static ssize_t connection_send(grpc_call *call, const uint8_t *data, size_t length);
static ssize_t h2_connection_send(h2_connection *connection, const uint8_t *data, size_t length, uint64_t deadline_abs_us, bool *timed_out);
static ssize_t connection_recv(h2_connection *connection, uint8_t *data, size_t length, uint64_t deadline_abs_us);
static int send_pending_h2_frames_with_deadline(h2_connection *connection, grpc_call *call, uint64_t fallback_deadline_abs_us);
static h2_connection *create_h2_connection(const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message);
static h2_connection *get_persistent_connection(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message);
static void discard_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
static int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us);
static const char *grpc_lite_trace_file_path(void);
static bool grpc_lite_trace_wire_bytes_enabled(void);
static void grpc_lite_trace_json_string(FILE *fp, const char *bytes, size_t len);
static void grpc_lite_trace_hex(FILE *fp, const uint8_t *bytes, size_t len);
static void grpc_lite_trace_sha256_hex(FILE *fp, const uint8_t *bytes, size_t len);
static bool grpc_lite_trace_header_value_is_sensitive(const uint8_t *name, size_t namelen);
static const char *grpc_lite_h2_frame_type_name(uint8_t type);
static const char *grpc_lite_h2_setting_name(uint32_t id);
static void grpc_lite_trace_open_and_lock(FILE **fp);
static void grpc_lite_trace_unlock_and_close(FILE *fp);
static void grpc_lite_trace_outbound_frame(h2_connection *connection, const uint8_t *data, size_t length);
static void grpc_lite_trace_inbound_frame(h2_connection *connection, const nghttp2_frame *frame);
static void grpc_lite_trace_inbound_frame_header(h2_connection *connection, const nghttp2_frame_hd *hd);
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static size_t remaining_request_bytes(grpc_call *call);
static size_t copy_request_bytes(grpc_call *call, uint8_t *buf, size_t length);
static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
static void grpc_protocol_set_message_header(grpc_call *call, size_t payload_len);
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);
static int on_begin_frame_callback(nghttp2_session *session, const nghttp2_frame_hd *hd, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static uint64_t monotonic_us(void);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
#endif
static int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen);
static int init_request_headers(h2_request_headers *headers);
static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen);
static void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us);
static void append_user_agent_request_header(h2_request_headers *headers, zend_string *primary_user_agent);
static int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);
static void free_request_headers(h2_request_headers *headers);
static int grpc_protocol_validate_response_message_lengths(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
static int grpc_protocol_process_response_data_direct(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
static bool server_streaming_read_ahead_limit_would_exceed(grpc_call *call, size_t payload_len);
static void mark_server_streaming_read_ahead_limit_exceeded(nghttp2_session *session, grpc_call *call);
static int enqueue_response_payload(nghttp2_session *session, grpc_call *call, zend_string *payload);
static void free_queued_response_payloads(grpc_call *call);
static void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call);
static int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
static void grpc_protocol_free_response_metadata_entries(grpc_call *call);
static void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing);
static void resolve_grpc_call_status(grpc_call *call, bool cancelled, grpc_lite_status_result *result);
static void add_status_result_to_return(zval *return_value, grpc_lite_status_result *status);
static void cleanup_grpc_call(grpc_call *call);

static grpc_call *grpc_call_from_stream_id(h2_connection *connection, int32_t stream_id)
{
    if (connection == NULL || connection->session == NULL || stream_id <= 0) {
        return NULL;
    }
    return (grpc_call *) nghttp2_session_get_stream_user_data(connection->session, stream_id);
}

static int register_grpc_call_stream(h2_connection *connection, grpc_call *call)
{
    if (connection == NULL || call == NULL || call->stream_id <= 0) {
        return FAILURE;
    }
    if (nghttp2_session_set_stream_user_data(connection->session, call->stream_id, call) != 0) {
        return FAILURE;
    }
    call->connection = connection;
    call->stream_registered = true;
    call->connection_owned = true;
    call->next_active_stream = connection->active_streams;
    connection->active_streams = call;
    connection->active_stream_count++;
    connection->stream_owner_count++;
    return SUCCESS;
}

static void mark_grpc_call_stream_registration_failed(h2_connection *connection, grpc_call *call)
{
    if (connection == NULL) {
        return;
    }
    if (call != NULL && call->stream_id > 0 && connection->session != NULL) {
        nghttp2_session_set_stream_user_data(connection->session, call->stream_id, NULL);
    }
    set_connection_error_detail(connection, "failed to register HTTP/2 stream");
    mark_connection_dead(connection, NGHTTP2_ERR_INVALID_ARGUMENT);
}

static void unregister_grpc_call_stream(grpc_call *call)
{
    h2_connection *connection;

    if (call == NULL || !call->stream_registered || call->connection == NULL || call->stream_id <= 0) {
        return;
    }
    connection = call->connection;
    if (connection->session != NULL) {
        nghttp2_session_set_stream_user_data(connection->session, call->stream_id, NULL);
    }
    if (connection->active_streams == call) {
        connection->active_streams = call->next_active_stream;
    } else {
        grpc_call *previous = connection->active_streams;
        while (previous != NULL && previous->next_active_stream != call) {
            previous = previous->next_active_stream;
        }
        if (previous != NULL) {
            previous->next_active_stream = call->next_active_stream;
        }
    }
    call->next_active_stream = NULL;
    call->stream_registered = false;
    if (connection->active_stream_count > 0) {
        connection->active_stream_count--;
    }
}

static void destroy_h2_connection(h2_connection *connection)
{
    if (connection == NULL) {
        return;
    }
    if (connection->ssl != NULL) {
        SSL_set_shutdown(connection->ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
        SSL_free(connection->ssl);
    }
    if (connection->ssl_ctx != NULL) {
        SSL_CTX_free(connection->ssl_ctx);
    }
    if (connection->fd >= 0) {
        close(connection->fd);
    }
    if (connection->write_buffer != NULL) {
        pefree(connection->write_buffer, connection->persistent);
    }
    if (connection->session != NULL) {
        nghttp2_session_del(connection->session);
    }
    if (connection->callbacks != NULL) {
        nghttp2_session_callbacks_del(connection->callbacks);
    }
    pefree(connection, connection->persistent);
}

static void destroy_detached_connection_if_unowned(h2_connection *connection)
{
    if (connection != NULL && connection->detached_from_cache && connection->stream_owner_count == 0) {
        destroy_h2_connection(connection);
    }
}

static void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection)
{
    if (entry == NULL) {
        return;
    }
    if (destroy_connection && entry->connection != NULL) {
        destroy_h2_connection(entry->connection);
    }
    if (entry->connection_key_identity != NULL) {
        zend_string_release_ex(entry->connection_key_identity, true);
    }
    pefree(entry, true);
}

static void detach_persistent_connection_by_ptr(h2_connection *connection)
{
    persistent_connection_entry *entry;
    persistent_connection_entry *matched_entry = NULL;
    zend_string *key;
    zend_string *matched_key = NULL;

    if (connection == NULL || !PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        return;
    }

    ZEND_HASH_FOREACH_STR_KEY_PTR(&PHP_GRPC_LITE_G(persistent_connections), key, entry) {
        if (key != NULL && entry != NULL && entry->connection == connection) {
            matched_entry = entry;
            matched_key = zend_string_copy(key);
            break;
        }
    } ZEND_HASH_FOREACH_END();

    if (matched_key != NULL) {
        zend_hash_del(&PHP_GRPC_LITE_G(persistent_connections), matched_key);
        zend_string_release(matched_key);
        destroy_persistent_connection_entry(matched_entry, false);
        connection->detached_from_cache = true;
    }
}

static bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *state)
{
    return connection != NULL && state != NULL && state->call.connection == connection;
}

static bool connection_owned_by_call(h2_connection *connection, grpc_call *call)
{
    return connection != NULL && call != NULL && call->connection == connection;
}

static void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *state)
{
    h2_connection *connection;

    if (state == NULL || state->call.connection == NULL) {
        return;
    }
    connection = state->call.connection;
    if (!connection_owned_by_server_streaming_call_state(connection, state)) {
        return;
    }

    if (!connection_usable(connection)) {
        detach_persistent_connection_by_ptr(connection);
    }
    unregister_grpc_call_stream(&state->call);
    if (state->call.connection_owned && connection->stream_owner_count > 0) {
        connection->stream_owner_count--;
    }
    state->call.connection_owned = false;
    state->call.connection = NULL;
    destroy_detached_connection_if_unowned(connection);
}

static void clear_connection_call_owner(h2_connection *connection, grpc_call *call)
{
    if (!connection_owned_by_call(connection, call)) {
        return;
    }
    unregister_grpc_call_stream(call);
    if (call->connection_owned && connection->stream_owner_count > 0) {
        connection->stream_owner_count--;
    }
    call->connection_owned = false;
}

static void cancel_active_server_streaming_call_state(server_streaming_call_state *state, uint32_t error_code)
{
    int rv;

    if (state == NULL || state->completed || !connection_owned_by_server_streaming_call_state(state->call.connection, state) || !connection_usable(state->call.connection) || state->call.stream_id <= 0) {
        return;
    }
    rv = nghttp2_submit_rst_stream(state->call.connection->session, NGHTTP2_FLAG_NONE, state->call.stream_id, error_code);
    if (rv != 0) {
        mark_connection_dead(state->call.connection, rv);
        return;
    }
    rv = send_pending_h2_frames(state->call.connection, &state->call);
    if (rv != 0) {
        mark_connection_dead(state->call.connection, rv);
    }
}

static void destroy_server_streaming_call_state(server_streaming_call_state *state)
{
    if (state == NULL) {
        return;
    }
    if (!state->completed && !state->call.stream_closed && connection_owned_by_server_streaming_call_state(state->call.connection, state) && connection_usable(state->call.connection) && state->call.stream_id > 0) {
        cancel_active_server_streaming_call_state(state, NGHTTP2_CANCEL);
        if (!connection_usable(state->call.connection)) {
            detach_persistent_connection_by_ptr(state->call.connection);
        }
    }
    clear_connection_server_streaming_call_state_owner(state);
    if (state->request != NULL) {
        zend_string_release(state->request);
    }
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    if (state->path != NULL) {
        zend_string_release(state->path);
    }
#endif
    if (state->recv_buf != NULL) {
        efree(state->recv_buf);
    }
    cleanup_grpc_call(&state->call);
    efree(state);
}

static void server_streaming_call_state_dtor(zend_resource *rsrc)
{
    destroy_server_streaming_call_state((server_streaming_call_state *) rsrc->ptr);
}

static int configure_callbacks(nghttp2_session_callbacks **callbacks)
{
    if (nghttp2_session_callbacks_new(callbacks) != 0) {
        return -1;
    }
    nghttp2_session_callbacks_set_send_callback(*callbacks, send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(*callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(*callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(*callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_begin_frame_callback(*callbacks, on_begin_frame_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(*callbacks, on_frame_recv_callback);
    return 0;
}

static const char *grpc_lite_trace_file_path(void)
{
    const char *path = getenv("GRPC_LITE_TRACE_FILE");
    return path != NULL && path[0] != '\0' ? path : NULL;
}

static bool grpc_lite_trace_wire_bytes_enabled(void)
{
    const char *value = getenv("GRPC_LITE_TRACE_WIRE_BYTES");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void grpc_lite_trace_json_string(FILE *fp, const char *bytes, size_t len)
{
    size_t i;
    fputc('"', fp);
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char) bytes[i];
        switch (ch) {
            case '\\':
                fputs("\\\\", fp);
                break;
            case '"':
                fputs("\\\"", fp);
                break;
            case '\b':
                fputs("\\b", fp);
                break;
            case '\f':
                fputs("\\f", fp);
                break;
            case '\n':
                fputs("\\n", fp);
                break;
            case '\r':
                fputs("\\r", fp);
                break;
            case '\t':
                fputs("\\t", fp);
                break;
            default:
                if (ch < 0x20) {
                    fprintf(fp, "\\u%04x", ch);
                } else {
                    fputc(ch, fp);
                }
                break;
        }
    }
    fputc('"', fp);
}

static void grpc_lite_trace_hex(FILE *fp, const uint8_t *bytes, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    fputc('"', fp);
    for (i = 0; i < len; i++) {
        fputc(hex[(bytes[i] >> 4) & 0x0f], fp);
        fputc(hex[bytes[i] & 0x0f], fp);
    }
    fputc('"', fp);
}

static void grpc_lite_trace_sha256_hex(FILE *fp, const uint8_t *bytes, size_t len)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(bytes, len, digest);
    grpc_lite_trace_hex(fp, digest, sizeof(digest));
}

static bool grpc_lite_trace_header_value_is_sensitive(const uint8_t *name, size_t namelen)
{
    return (namelen == sizeof("authorization") - 1 && memcmp(name, "authorization", sizeof("authorization") - 1) == 0)
        || (namelen == sizeof("x-goog-user-project") - 1 && memcmp(name, "x-goog-user-project", sizeof("x-goog-user-project") - 1) == 0)
        || (namelen == sizeof("x-goog-request-params") - 1 && memcmp(name, "x-goog-request-params", sizeof("x-goog-request-params") - 1) == 0)
        || (namelen == sizeof("google-cloud-resource-prefix") - 1 && memcmp(name, "google-cloud-resource-prefix", sizeof("google-cloud-resource-prefix") - 1) == 0);
}

static const char *grpc_lite_h2_frame_type_name(uint8_t type)
{
    switch (type) {
        case NGHTTP2_DATA:
            return "DATA";
        case NGHTTP2_HEADERS:
            return "HEADERS";
        case NGHTTP2_PRIORITY:
            return "PRIORITY";
        case NGHTTP2_RST_STREAM:
            return "RST_STREAM";
        case NGHTTP2_SETTINGS:
            return "SETTINGS";
        case NGHTTP2_PUSH_PROMISE:
            return "PUSH_PROMISE";
        case NGHTTP2_PING:
            return "PING";
        case NGHTTP2_GOAWAY:
            return "GOAWAY";
        case NGHTTP2_WINDOW_UPDATE:
            return "WINDOW_UPDATE";
        case NGHTTP2_CONTINUATION:
            return "CONTINUATION";
        default:
            return "UNKNOWN";
    }
}

static const char *grpc_lite_h2_setting_name(uint32_t id)
{
    switch (id) {
        case NGHTTP2_SETTINGS_HEADER_TABLE_SIZE:
            return "HEADER_TABLE_SIZE";
        case NGHTTP2_SETTINGS_ENABLE_PUSH:
            return "ENABLE_PUSH";
        case NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
            return "MAX_CONCURRENT_STREAMS";
        case NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE:
            return "INITIAL_WINDOW_SIZE";
        case NGHTTP2_SETTINGS_MAX_FRAME_SIZE:
            return "MAX_FRAME_SIZE";
        case NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
            return "MAX_HEADER_LIST_SIZE";
        default:
            return "UNKNOWN";
    }
}

static void grpc_lite_trace_open_and_lock(FILE **fp)
{
    const char *path = grpc_lite_trace_file_path();
    if (path == NULL) {
        *fp = NULL;
        return;
    }
    *fp = fopen(path, "a");
    if (*fp != NULL) {
        flock(fileno(*fp), LOCK_EX);
    }
}

static void grpc_lite_trace_unlock_and_close(FILE *fp)
{
    if (fp == NULL) {
        return;
    }
    flock(fileno(fp), LOCK_UN);
    fclose(fp);
}

static uint32_t grpc_lite_read_be32(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << 24)
        | ((uint32_t) bytes[1] << 16)
        | ((uint32_t) bytes[2] << 8)
        | (uint32_t) bytes[3];
}

static uint16_t grpc_lite_read_be16(const uint8_t *bytes)
{
    return (uint16_t) (((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1]);
}

static void grpc_lite_trace_settings_entries_from_payload(FILE *fp, const uint8_t *payload, uint32_t payload_len)
{
    uint32_t count;
    uint32_t index;

    if (payload == NULL || payload_len % 6 != 0) {
        return;
    }
    count = payload_len / 6;
    fprintf(fp, ",\"settings_count\":%u,\"settings\":[", count);
    for (index = 0; index < count; index++) {
        const uint8_t *entry = payload + (index * 6);
        uint32_t id = grpc_lite_read_be16(entry);
        uint32_t value = grpc_lite_read_be32(entry + 2);
        if (index > 0) {
            fputc(',', fp);
        }
        fprintf(fp, "{\"id\":%u,\"name\":", id);
        grpc_lite_trace_json_string(fp, grpc_lite_h2_setting_name(id), strlen(grpc_lite_h2_setting_name(id)));
        fprintf(fp, ",\"value\":%u}", value);
    }
    fputc(']', fp);
}

static void grpc_lite_trace_settings_entries_from_frame(FILE *fp, const nghttp2_settings *settings)
{
    size_t index;

    if (settings == NULL) {
        return;
    }
    fprintf(fp, ",\"settings_count\":%zu,\"settings\":[", settings->niv);
    for (index = 0; index < settings->niv; index++) {
        uint32_t id = (uint32_t) settings->iv[index].settings_id;
        if (index > 0) {
            fputc(',', fp);
        }
        fprintf(fp, "{\"id\":%u,\"name\":", id);
        grpc_lite_trace_json_string(fp, grpc_lite_h2_setting_name(id), strlen(grpc_lite_h2_setting_name(id)));
        fprintf(fp, ",\"value\":%u}", settings->iv[index].value);
    }
    fputc(']', fp);
}

static void grpc_lite_trace_outbound_frame_record(h2_connection *connection, const uint8_t *data, size_t length)
{
    FILE *fp;
    uint32_t frame_len;
    uint8_t type;
    uint8_t flags;
    int32_t stream_id;
    grpc_call *call;
    const char *frame_type_name;

    if (connection == NULL || data == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    if (length < 9) {
        return;
    }

    frame_len = ((uint32_t) data[0] << 16) | ((uint32_t) data[1] << 8) | (uint32_t) data[2];
    type = data[3];
    flags = data[4];
    stream_id = (int32_t) ((((uint32_t) data[5] & 0x7f) << 24) | ((uint32_t) data[6] << 16) | ((uint32_t) data[7] << 8) | (uint32_t) data[8]);
    call = grpc_call_from_stream_id(connection, stream_id);
    frame_type_name = grpc_lite_h2_frame_type_name(type);

    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.frame_out\",\"stream_id\":%d,\"frame_type\":", monotonic_us(), (long) getpid(), stream_id);
    grpc_lite_trace_json_string(fp, frame_type_name, strlen(frame_type_name));
    fprintf(fp, ",\"frame_type_id\":%u,\"flags\":%u,\"frame_payload_len\":%u,\"chunk_len\":%zu", (unsigned) type, (unsigned) flags, frame_len, length);
    if (stream_id > 0 && call != NULL && call->method_path != NULL) {
        fprintf(fp, ",\"rpc_method\":");
        grpc_lite_trace_json_string(fp, ZSTR_VAL(call->method_path), ZSTR_LEN(call->method_path));
    }
    if (type == NGHTTP2_SETTINGS && length >= 9 + frame_len) {
        grpc_lite_trace_settings_entries_from_payload(fp, data + 9, frame_len);
    } else if (type == NGHTTP2_WINDOW_UPDATE && length >= 13 && frame_len == 4) {
        fprintf(fp, ",\"window_size_increment\":%u", grpc_lite_read_be32(data + 9) & 0x7fffffffU);
    }
    if (type == NGHTTP2_HEADERS) {
        fprintf(fp, ",\"header_block_len\":%u", frame_len);
        if (grpc_lite_trace_wire_bytes_enabled()) {
            size_t available = length - 9;
            size_t header_bytes = frame_len < available ? frame_len : available;
            fprintf(fp, ",\"header_block_hex\":");
            grpc_lite_trace_hex(fp, data + 9, header_bytes);
        }
    } else if (grpc_lite_trace_wire_bytes_enabled()
        && (type == NGHTTP2_SETTINGS || type == NGHTTP2_WINDOW_UPDATE || type == NGHTTP2_PING || type == NGHTTP2_GOAWAY)
        && length >= 9 + frame_len) {
        fprintf(fp, ",\"payload_hex\":");
        grpc_lite_trace_hex(fp, data + 9, frame_len);
    }
    fputs("}\n", fp);
    grpc_lite_trace_unlock_and_close(fp);
}

static void grpc_lite_trace_outbound_frame(h2_connection *connection, const uint8_t *data, size_t length)
{
    FILE *fp;
    size_t offset = 0;

    if (connection == NULL || data == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    if (length >= 24 && memcmp(data, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0) {
        grpc_lite_trace_open_and_lock(&fp);
        if (fp == NULL) {
            return;
        }
        fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.connection_preface\",\"chunk_len\":24}\n", monotonic_us(), (long) getpid());
        grpc_lite_trace_unlock_and_close(fp);
        offset = 24;
    }
    while (offset + 9 <= length) {
        uint32_t frame_len = ((uint32_t) data[offset] << 16) | ((uint32_t) data[offset + 1] << 8) | (uint32_t) data[offset + 2];
        size_t frame_total_len = (size_t) frame_len + 9;
        if (frame_total_len < 9 || offset + frame_total_len > length) {
            break;
        }
        grpc_lite_trace_outbound_frame_record(connection, data + offset, frame_total_len);
        offset += frame_total_len;
    }
    if (offset < length) {
        grpc_lite_trace_open_and_lock(&fp);
        if (fp == NULL) {
            return;
        }
        fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.chunk_unparsed\",\"chunk_len\":%zu,\"offset\":%zu,\"remaining_len\":%zu}\n", monotonic_us(), (long) getpid(), length, offset, length - offset);
        grpc_lite_trace_unlock_and_close(fp);
    }
}

static void grpc_lite_trace_inbound_frame(h2_connection *connection, const nghttp2_frame *frame)
{
    FILE *fp;
    grpc_call *call;

    if (connection == NULL || frame == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    call = grpc_call_from_stream_id(connection, frame->hd.stream_id);
    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.frame_in\",\"stream_id\":%d,\"frame_type\":", monotonic_us(), (long) getpid(), frame->hd.stream_id);
    grpc_lite_trace_json_string(fp, grpc_lite_h2_frame_type_name(frame->hd.type), strlen(grpc_lite_h2_frame_type_name(frame->hd.type)));
    fprintf(fp, ",\"frame_type_id\":%u,\"flags\":%u,\"frame_payload_len\":%zu", (unsigned) frame->hd.type, (unsigned) frame->hd.flags, frame->hd.length);
    if (call != NULL && call->method_path != NULL) {
        fprintf(fp, ",\"rpc_method\":");
        grpc_lite_trace_json_string(fp, ZSTR_VAL(call->method_path), ZSTR_LEN(call->method_path));
    }
    if (frame->hd.type == NGHTTP2_SETTINGS) {
        grpc_lite_trace_settings_entries_from_frame(fp, &frame->settings);
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        fprintf(fp, ",\"window_size_increment\":%d", frame->window_update.window_size_increment);
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
        fprintf(fp, ",\"last_stream_id\":%d,\"error_code\":%u", frame->goaway.last_stream_id, frame->goaway.error_code);
    } else if (frame->hd.type == NGHTTP2_RST_STREAM) {
        fprintf(fp, ",\"error_code\":%u", frame->rst_stream.error_code);
    } else if (frame->hd.type == NGHTTP2_PING && grpc_lite_trace_wire_bytes_enabled()) {
        fprintf(fp, ",\"payload_hex\":");
        grpc_lite_trace_hex(fp, frame->ping.opaque_data, sizeof(frame->ping.opaque_data));
    }
    fputs("}\n", fp);
    grpc_lite_trace_unlock_and_close(fp);
}

static void grpc_lite_trace_inbound_frame_header(h2_connection *connection, const nghttp2_frame_hd *hd)
{
    FILE *fp;
    grpc_call *call;

    if (connection == NULL || hd == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    if (hd->type != NGHTTP2_HEADERS && hd->type != NGHTTP2_DATA) {
        return;
    }

    call = grpc_call_from_stream_id(connection, hd->stream_id);
    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.frame_in\",\"stream_id\":%d,\"frame_type\":", monotonic_us(), (long) getpid(), hd->stream_id);
    grpc_lite_trace_json_string(fp, grpc_lite_h2_frame_type_name(hd->type), strlen(grpc_lite_h2_frame_type_name(hd->type)));
    fprintf(fp, ",\"frame_type_id\":%u,\"flags\":%u,\"frame_payload_len\":%zu", (unsigned) hd->type, (unsigned) hd->flags, hd->length);
    if (call != NULL && call->method_path != NULL) {
        fprintf(fp, ",\"rpc_method\":");
        grpc_lite_trace_json_string(fp, ZSTR_VAL(call->method_path), ZSTR_LEN(call->method_path));
    }
    fputs("}\n", fp);
    grpc_lite_trace_unlock_and_close(fp);
}

static void grpc_lite_trace_transport_io(h2_connection *connection, const char *event, size_t requested_len, ssize_t result_len, int ssl_error, int saved_errno)
{
    FILE *fp;

    if (connection == NULL || event == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":", monotonic_us(), (long) getpid());
    grpc_lite_trace_json_string(fp, event, strlen(event));
    fprintf(fp, ",\"requested_len\":%zu,\"result_len\":%zd", requested_len, result_len);
    if (ssl_error != 0) {
        fprintf(fp, ",\"ssl_error\":%d", ssl_error);
    }
    if (saved_errno != 0) {
        fprintf(fp, ",\"errno\":%d", saved_errno);
    }
    if (connection->current_io_call != NULL && connection->current_io_call->method_path != NULL) {
        fprintf(fp, ",\"rpc_method\":");
        grpc_lite_trace_json_string(fp, ZSTR_VAL(connection->current_io_call->method_path), ZSTR_LEN(connection->current_io_call->method_path));
        fprintf(fp, ",\"stream_id\":%d", connection->current_io_call->stream_id);
    }
    fputs("}\n", fp);
    grpc_lite_trace_unlock_and_close(fp);
}

static void grpc_lite_trace_persistent_preflight(h2_connection *connection, const char *result, const char *reason, ssize_t peek_result, int ssl_error, int saved_errno)
{
    FILE *fp;

    if (connection == NULL || result == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"persistent.preflight\",\"result\":", monotonic_us(), (long) getpid());
    grpc_lite_trace_json_string(fp, result, strlen(result));
    fprintf(fp, ",\"reason\":");
    grpc_lite_trace_json_string(fp, reason != NULL ? reason : "", reason != NULL ? strlen(reason) : 0);
    fprintf(
        fp,
        ",\"fd\":%d,\"active_stream_count\":%zu,\"dead\":%s,\"draining\":%s,\"peek_result\":%zd,\"ssl_error\":%d,\"errno\":%d}\n",
        connection->fd,
        connection->active_stream_count,
        connection->dead ? "true" : "false",
        connection->draining ? "true" : "false",
        peek_result,
        ssl_error,
        saved_errno
    );
    grpc_lite_trace_unlock_and_close(fp);
}

static void grpc_lite_trace_request_headers(grpc_call *call, const nghttp2_nv *headers, size_t header_count)
{
    FILE *fp;
    size_t i;

    if (call == NULL || call->method_path == NULL || headers == NULL || grpc_lite_trace_file_path() == NULL) {
        return;
    }
    grpc_lite_trace_open_and_lock(&fp);
    if (fp == NULL) {
        return;
    }
    for (i = 0; i < header_count; i++) {
        bool sensitive = grpc_lite_trace_header_value_is_sensitive(headers[i].name, headers[i].namelen);
        fprintf(fp, "{\"monotonic_us\":%" PRIu64 ",\"pid\":%ld,\"event\":\"wire.request_header\",\"stream_id\":%d,\"rpc_method\":", monotonic_us(), (long) getpid(), call->stream_id);
        grpc_lite_trace_json_string(fp, ZSTR_VAL(call->method_path), ZSTR_LEN(call->method_path));
        fprintf(fp, ",\"index\":%zu,\"name\":", i);
        grpc_lite_trace_json_string(fp, (const char *) headers[i].name, headers[i].namelen);
        fprintf(fp, ",\"value_len\":%zu,\"flags\":%u,\"sensitive\":%s", headers[i].valuelen, (unsigned) headers[i].flags, sensitive ? "true" : "false");
        if (sensitive) {
            fprintf(fp, ",\"value_sha256\":");
            grpc_lite_trace_sha256_hex(fp, headers[i].value, headers[i].valuelen);
        } else {
            fprintf(fp, ",\"value\":");
            grpc_lite_trace_json_string(fp, (const char *) headers[i].value, headers[i].valuelen);
        }
        fputs("}\n", fp);
    }
    grpc_lite_trace_unlock_and_close(fp);
}

static void mark_connection_dead(h2_connection *connection, int error_code)
{
    if (connection == NULL) {
        return;
    }
    connection->dead = true;
    connection->last_error = error_code;
    if (error_code > 0) {
        connection->last_io_errno = error_code;
        if (connection->last_error_detail[0] == '\0') {
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "%s", strerror(error_code));
        }
    } else if (error_code < 0) {
        if (connection->last_error_detail[0] == '\0') {
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "nghttp2 error: %s", nghttp2_strerror(error_code));
        }
    } else if (connection->last_error_detail[0] == '\0') {
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "connection closed");
    }
}

static void set_connection_error_detail(h2_connection *connection, const char *detail)
{
    if (connection == NULL || detail == NULL) {
        return;
    }
    snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "%s", detail);
}

static void mark_connection_draining(h2_connection *connection, int32_t last_stream_id, uint32_t error_code)
{
    if (connection == NULL) {
        return;
    }
    connection->draining = true;
    connection->last_goaway_stream_id = last_stream_id;
    connection->last_goaway_error_code = error_code;
}

static bool connection_usable(h2_connection *connection)
{
    return connection != NULL && connection->fd >= 0 && connection->session != NULL && !connection->dead && !connection->draining;
}

static bool identity_matches(zend_string *stored, const char *expected, size_t expected_len)
{
    if (stored == NULL) {
        return false;
    }
    if (expected == NULL) {
        expected = "";
    }
    return ZSTR_LEN(stored) == expected_len && memcmp(ZSTR_VAL(stored), expected, expected_len) == 0;
}

static persistent_connection_entry *create_persistent_connection_entry(h2_connection *connection, const char *key, size_t key_len)
{
    persistent_connection_entry *entry;

    if (connection == NULL) {
        return NULL;
    }

    entry = pecalloc(1, sizeof(persistent_connection_entry), true);
    if (entry == NULL) {
        return NULL;
    }

    entry->connection = connection;
    entry->connection_key_identity = zend_string_init(key, key_len, true);

    if (entry->connection_key_identity == NULL) {
        destroy_persistent_connection_entry(entry, false);
        return NULL;
    }

    return entry;
}

static bool connection_entry_matches_key(persistent_connection_entry *entry, const char *key, size_t key_len)
{
    if (entry == NULL || entry->connection == NULL) {
        return false;
    }
    return identity_matches(entry->connection_key_identity, key, key_len);
}

static bool drain_pending_connection_data_for_reuse(h2_connection *connection, uint64_t deadline_abs_us)
{
    uint8_t buffer[GRPC_LITE_PREFLIGHT_DRAIN_CHUNK_SIZE];
    size_t total_read = 0;
    size_t iterations = 0;
    bool reached_read_boundary = false;

    if (!connection_usable(connection)) {
        return false;
    }

    while (iterations < GRPC_LITE_PREFLIGHT_DRAIN_MAX_ITERATIONS && total_read < GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES) {
        ssize_t nread;
        iterations++;

        if (connection->ssl != NULL) {
            int ssl_read = SSL_read(connection->ssl, buffer, sizeof(buffer));
            if (ssl_read > 0) {
                nread = ssl_read;
                grpc_lite_trace_transport_io(connection, "wire.tls_preflight_read", sizeof(buffer), nread, 0, 0);
            } else {
                int ssl_error = SSL_get_error(connection->ssl, ssl_read);
                grpc_lite_trace_transport_io(connection, "wire.tls_preflight_read_retry", sizeof(buffer), ssl_read, ssl_error, 0);
                connection->last_ssl_error = ssl_error;
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                    reached_read_boundary = true;
                    break;
                }
                if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                    set_connection_error_detail(connection, "persistent TLS connection closed by peer before reuse");
                    mark_connection_dead(connection, 0);
                } else {
                    snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "persistent TLS connection preflight read failed: SSL_get_error=%d", ssl_error);
                    mark_connection_dead(connection, ECONNRESET);
                }
                return false;
            }
        } else {
            nread = recv(connection->fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (nread == 0) {
                set_connection_error_detail(connection, "persistent connection closed by peer before reuse");
                mark_connection_dead(connection, 0);
                return false;
            }
            if (nread < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    reached_read_boundary = true;
                    break;
                }
                mark_connection_dead(connection, errno);
                return false;
            }
            grpc_lite_trace_transport_io(connection, "wire.socket_preflight_read", sizeof(buffer), nread, 0, 0);
        }

        total_read += (size_t) nread;
        ssize_t rv = nghttp2_session_mem_recv(connection->session, buffer, (size_t) nread);
        if (rv < 0) {
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "nghttp2_session_mem_recv failed during persistent preflight: %s", nghttp2_strerror((int) rv));
            mark_connection_dead(connection, (int) rv);
            return false;
        }
        if (!connection_usable(connection)) {
            return false;
        }
        if (nghttp2_session_want_write(connection->session)) {
            rv = send_pending_h2_frames_with_deadline(connection, NULL, deadline_abs_us);
            if (rv != 0) {
                if (connection->last_error_detail[0] == '\0') {
                    snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "nghttp2_session_send failed during persistent preflight: %s", nghttp2_strerror((int) rv));
                }
                mark_connection_dead(connection, (int) rv);
                return false;
            }
        }
    }

    if (!reached_read_boundary) {
        set_connection_error_detail(connection, "persistent preflight drain limit exceeded");
        mark_connection_draining(connection, 0, NGHTTP2_NO_ERROR);
        return false;
    }

    return connection_usable(connection);
}

static bool preflight_persistent_connection(h2_connection *connection, uint64_t deadline_abs_us)
{
    char byte;
    ssize_t rv;

    if (!connection_usable(connection)) {
        grpc_lite_trace_persistent_preflight(connection, "reject", "connection is not usable", -1, 0, 0);
        return connection_usable(connection);
    }
    if (connection->active_stream_count > 0) {
        grpc_lite_trace_persistent_preflight(connection, "accept", "active streams present", -1, 0, 0);
        return true;
    }
    if (connection->ssl != NULL) {
        int ssl_error;
        rv = SSL_peek(connection->ssl, &byte, sizeof(byte));
        ssl_error = SSL_get_error(connection->ssl, (int) rv);
        if (rv > 0) {
            bool drained = drain_pending_connection_data_for_reuse(connection, deadline_abs_us);
            grpc_lite_trace_persistent_preflight(
                connection,
                drained ? "accept" : "reject",
                drained ? "pending TLS data drained" : connection->last_error_detail,
                rv,
                ssl_error,
                0
            );
            return drained;
        }
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            grpc_lite_trace_persistent_preflight(connection, "accept", "no pending TLS data", rv, ssl_error, 0);
            return true;
        }
        connection->last_ssl_error = ssl_error;
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            set_connection_error_detail(connection, "persistent TLS connection closed by peer before reuse");
            mark_connection_dead(connection, 0);
        } else {
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "persistent TLS connection preflight failed: SSL_get_error=%d", ssl_error);
            mark_connection_dead(connection, ECONNRESET);
        }
        grpc_lite_trace_persistent_preflight(connection, "reject", connection->last_error_detail, rv, ssl_error, 0);
        return false;
    }

    rv = recv(connection->fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (rv == 0) {
        set_connection_error_detail(connection, "persistent connection closed by peer before reuse");
        mark_connection_dead(connection, 0);
        grpc_lite_trace_persistent_preflight(connection, "reject", connection->last_error_detail, rv, 0, 0);
        return false;
    }
    if (rv < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        int saved_errno = errno;
        mark_connection_dead(connection, errno);
        grpc_lite_trace_persistent_preflight(connection, "reject", connection->last_error_detail, rv, 0, saved_errno);
        return false;
    }
    if (rv > 0) {
        bool drained = drain_pending_connection_data_for_reuse(connection, deadline_abs_us);
        grpc_lite_trace_persistent_preflight(
            connection,
            drained ? "accept" : "reject",
            drained ? "pending socket data drained" : connection->last_error_detail,
            rv,
            0,
            0
        );
        return drained;
    }

    grpc_lite_trace_persistent_preflight(connection, "accept", "no pending socket data", rv, 0, 0);
    return true;
}

static void remove_unusable_persistent_connection(const char *key, size_t key_len, h2_connection *connection)
{
    persistent_connection_entry *entry = NULL;

    if (connection == NULL || connection_usable(connection)) {
        return;
    }
    if (PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        entry = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_connections), key, key_len);
        zend_hash_str_del(&PHP_GRPC_LITE_G(persistent_connections), key, key_len);
    }
    if (connection->stream_owner_count > 0) {
        connection->detached_from_cache = true;
        destroy_persistent_connection_entry(entry, false);
        return;
    }
    if (entry != NULL && entry->connection == connection) {
        destroy_persistent_connection_entry(entry, true);
        return;
    }
    destroy_h2_connection(connection);
}

static int set_fd_nonblocking_mode(int fd, bool nonblocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags);
}

static int poll_timeout_ms_for_deadline(uint64_t deadline_abs_us)
{
    if (deadline_abs_us == 0) {
        return -1;
    }
    uint64_t now = monotonic_us();
    if (now >= deadline_abs_us) {
        return 0;
    }
    uint64_t remaining_us = deadline_abs_us - now;
    uint64_t remaining_ms = (remaining_us + 999) / 1000;
    return remaining_ms > INT_MAX ? INT_MAX : (int) remaining_ms;
}

static zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us)
{
    if (deadline_abs_us == 0) {
        return 0;
    }
    uint64_t now = monotonic_us();
    if (now >= deadline_abs_us) {
        return -1;
    }
    uint64_t remaining = deadline_abs_us - now;
    return remaining > (uint64_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) remaining;
}

static size_t effective_server_streaming_read_ahead_max_messages(void)
{
    return PHP_GRPC_LITE_G(server_streaming_read_ahead_max_messages) > 0
        ? (size_t) PHP_GRPC_LITE_G(server_streaming_read_ahead_max_messages)
        : GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_MESSAGES;
}

static size_t effective_server_streaming_read_ahead_max_bytes(void)
{
    return PHP_GRPC_LITE_G(server_streaming_read_ahead_max_bytes) > 0
        ? (size_t) PHP_GRPC_LITE_G(server_streaming_read_ahead_max_bytes)
        : GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_BYTES;
}

static int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us)
{
    while (true) {
        struct pollfd pfd;
        int timeout_ms = poll_timeout_ms_for_deadline(deadline_abs_us);
        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        int rv = poll(&pfd, 1, timeout_ms);
        if (rv > 0) {
            return (pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL)) ? 0 : -1;
        }
        if (rv == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len)
{
    if (pem_len > INT_MAX) {
        return -1;
    }
    BIO *bio = BIO_new_mem_buf(pem, (int) pem_len);
    if (bio == NULL) {
        return -1;
    }
    int loaded = 0;
    while (true) {
        X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (cert == NULL) {
            break;
        }
        if (X509_STORE_add_cert(store, cert) == 1) {
            loaded++;
        }
        X509_free(cert);
    }
    BIO_free(bio);
    ERR_clear_error();
    return loaded > 0 ? 0 : -1;
}

static int configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len)
{
    if (cert_len > INT_MAX || key_len > INT_MAX) {
        return -1;
    }
    BIO *cert_bio = BIO_new_mem_buf(cert, (int) cert_len);
    BIO *key_bio = BIO_new_mem_buf(key, (int) key_len);
    X509 *x509 = NULL;
    EVP_PKEY *pkey = NULL;
    int ok = 0;
    if (cert_bio == NULL || key_bio == NULL) {
        if (cert_bio != NULL) {
            BIO_free(cert_bio);
        }
        if (key_bio != NULL) {
            BIO_free(key_bio);
        }
        return -1;
    }

    x509 = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    pkey = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(key_bio);
    if (x509 == NULL || pkey == NULL) {
        if (x509 != NULL) {
            X509_free(x509);
        }
        if (pkey != NULL) {
            EVP_PKEY_free(pkey);
        }
        BIO_free(cert_bio);
        return -1;
    }

    ok = SSL_CTX_use_certificate(ctx, x509) == 1
        && SSL_CTX_use_PrivateKey(ctx, pkey) == 1
        && SSL_CTX_check_private_key(ctx) == 1;
    X509_free(x509);
    EVP_PKEY_free(pkey);
    if (!ok) {
        BIO_free(cert_bio);
        return -1;
    }

    while (true) {
        X509 *chain_cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
        if (chain_cert == NULL) {
            break;
        }
        if (SSL_CTX_add_extra_chain_cert(ctx, chain_cert) != 1) {
            X509_free(chain_cert);
            BIO_free(cert_bio);
            return -1;
        }
    }
    BIO_free(cert_bio);
    ERR_clear_error();
    return 0;
}

static int configure_tls_connection(h2_connection *connection, const char *host, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us)
{
    const char *verify_name = tls_verify_name != NULL && tls_verify_name_len > 0 ? tls_verify_name : host;

    connection->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (connection->ssl_ctx == NULL) {
        set_connection_error_detail(connection, "failed to create SSL_CTX");
        return -1;
    }
    if (SSL_CTX_set_min_proto_version(connection->ssl_ctx, TLS1_2_VERSION) != 1) {
        set_connection_error_detail(connection, "failed to set TLS minimum protocol version");
        return -1;
    }
    SSL_CTX_set_options(connection->ssl_ctx, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(connection->ssl_ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_CTX_set_verify(connection->ssl_ctx, SSL_VERIFY_PEER, NULL);

    if (root_certs != NULL && root_certs_len > 0) {
        if (add_pem_certs_to_store(SSL_CTX_get_cert_store(connection->ssl_ctx), root_certs, root_certs_len) != 0) {
            set_connection_error_detail(connection, "failed to load root certificates");
            return -1;
        }
    } else if (SSL_CTX_set_default_verify_paths(connection->ssl_ctx) != 1) {
        set_connection_error_detail(connection, "failed to load default root certificates");
        return -1;
    }

    if ((cert_chain != NULL && cert_chain_len > 0) != (private_key != NULL && private_key_len > 0)) {
        set_connection_error_detail(connection, "client certificate and private key must be configured together");
        return -1;
    }
    if (cert_chain != NULL && private_key != NULL && cert_chain_len > 0 && private_key_len > 0) {
        if (configure_client_certificate(connection->ssl_ctx, cert_chain, cert_chain_len, private_key, private_key_len) != 0) {
            set_connection_error_detail(connection, "failed to configure client certificate");
            return -1;
        }
    }

    connection->ssl = SSL_new(connection->ssl_ctx);
    if (connection->ssl == NULL) {
        set_connection_error_detail(connection, "failed to create SSL object");
        return -1;
    }
    SSL_set_read_ahead(connection->ssl, 1);
    static const unsigned char alpn[] = {2, 'h', '2'};
    if (SSL_set_alpn_protos(connection->ssl, alpn, sizeof(alpn)) != 0) {
        set_connection_error_detail(connection, "failed to configure TLS ALPN h2");
        return -1;
    }
    if (SSL_set_tlsext_host_name(connection->ssl, verify_name) != 1) {
        set_connection_error_detail(connection, "failed to configure TLS SNI host");
        return -1;
    }
    if (SSL_set1_host(connection->ssl, verify_name) != 1) {
        set_connection_error_detail(connection, "failed to configure TLS verification host");
        return -1;
    }
    if (SSL_set_fd(connection->ssl, connection->fd) != 1) {
        set_connection_error_detail(connection, "failed to attach TLS socket");
        return -1;
    }
    if (set_fd_nonblocking_mode(connection->fd, true) != 0) {
        set_connection_error_detail(connection, "failed to set TLS socket nonblocking");
        return -1;
    }
    while (true) {
        int rv = SSL_connect(connection->ssl);
        if (rv == 1) {
            break;
        }
        int ssl_error = SSL_get_error(connection->ssl, rv);
        connection->last_ssl_error = ssl_error;
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
            if (poll_fd_until_deadline(connection->fd, events, deadline_abs_us) == 0) {
                continue;
            }
            connection->last_io_errno = errno;
            if (errno == ETIMEDOUT) {
                set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
            } else {
                snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "TLS handshake poll failed: %s", strerror(errno));
            }
            set_fd_nonblocking_mode(connection->fd, false);
            return -1;
        }
        connection->tls_verify_result = SSL_get_verify_result(connection->ssl);
        if (connection->tls_verify_result != X509_V_OK) {
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "TLS certificate verification failed: %s", X509_verify_cert_error_string(connection->tls_verify_result));
        } else {
            unsigned long err = ERR_get_error();
            if (err != 0) {
                ERR_error_string_n(err, connection->last_error_detail, sizeof(connection->last_error_detail));
            } else {
                snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "TLS handshake failed: SSL_get_error=%d", ssl_error);
            }
        }
        set_fd_nonblocking_mode(connection->fd, false);
        return -1;
    }
    connection->tls_verify_result = SSL_get_verify_result(connection->ssl);
    if (connection->tls_verify_result != X509_V_OK) {
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "TLS certificate verification failed: %s", X509_verify_cert_error_string(connection->tls_verify_result));
        return -1;
    }
    const unsigned char *selected_alpn = NULL;
    unsigned int selected_alpn_len = 0;
    SSL_get0_alpn_selected(connection->ssl, &selected_alpn, &selected_alpn_len);
    if (selected_alpn_len != 2 || selected_alpn == NULL || memcmp(selected_alpn, "h2", 2) != 0) {
        if (selected_alpn_len > 0 && selected_alpn != NULL) {
            size_t copy_len = selected_alpn_len < sizeof(connection->negotiated_protocol) - 1 ? selected_alpn_len : sizeof(connection->negotiated_protocol) - 1;
            memcpy(connection->negotiated_protocol, selected_alpn, copy_len);
            connection->negotiated_protocol[copy_len] = '\0';
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "TLS ALPN did not negotiate h2: %s", connection->negotiated_protocol);
        } else {
            set_connection_error_detail(connection, "TLS ALPN did not negotiate h2");
        }
        return -1;
    }
    snprintf(connection->negotiated_protocol, sizeof(connection->negotiated_protocol), "h2");
    connection->tls = true;
    return 0;
}

static ssize_t connection_send(grpc_call *call, const uint8_t *data, size_t length)
{
    ssize_t written;
    if (call == NULL) {
        errno = EINVAL;
        return -1;
    }
    written = h2_connection_send(call->connection, data, length, call->deadline_abs_us, &call->timed_out);
    if (written < 0) {
        call->last_io_errno = errno;
        if (call->connection != NULL) {
            call->last_ssl_error = call->connection->last_ssl_error;
            snprintf(call->last_io_error_detail, sizeof(call->last_io_error_detail), "%s", call->connection->last_error_detail);
        }
    }
    return written;
}

static ssize_t h2_connection_send(h2_connection *connection, const uint8_t *data, size_t length, uint64_t deadline_abs_us, bool *timed_out)
{
    zend_long remaining_timeout_us;
    if (connection == NULL) {
        errno = EINVAL;
        return -1;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        errno = ETIMEDOUT;
        if (timed_out != NULL) {
            *timed_out = true;
        }
        connection->last_io_errno = errno;
        set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
        return -1;
    }
    if (connection->ssl != NULL) {
        if (length > INT_MAX) {
            errno = EMSGSIZE;
            connection->last_io_errno = errno;
            set_connection_error_detail(connection, "SSL_write length exceeds INT_MAX");
            return -1;
        }
        while (true) {
            int written = SSL_write(connection->ssl, data, (int) length);
            if (written > 0) {
                grpc_lite_trace_transport_io(connection, "wire.tls_write", length, written, 0, 0);
                return written;
            }
            int ssl_error = SSL_get_error(connection->ssl, written);
            grpc_lite_trace_transport_io(connection, "wire.tls_write_retry", length, written, ssl_error, 0);
            connection->last_ssl_error = ssl_error;
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                int poll_result = poll_fd_until_deadline(connection->fd, events, deadline_abs_us);
                if (poll_result == 0) {
                    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
                    if (remaining_timeout_us >= 0) {
                        continue;
                    }
                    errno = ETIMEDOUT;
                }
                if (errno != ETIMEDOUT) {
                    connection->last_io_errno = errno;
                    snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_write poll failed: %s", strerror(errno));
                    return -1;
                }
                if (timed_out != NULL) {
                    *timed_out = true;
                }
                connection->last_io_errno = errno;
                set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
                return -1;
            }
            errno = ECONNRESET;
            connection->last_io_errno = errno;
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_write failed: SSL_get_error=%d", ssl_error);
            return -1;
        }
    }
    while (true) {
        ssize_t written = send(connection->fd, data, length,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (written >= 0) {
            grpc_lite_trace_transport_io(connection, "wire.socket_write", length, written, 0, 0);
            return written;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int poll_result = poll_fd_until_deadline(connection->fd, POLLOUT, deadline_abs_us);
            if (poll_result == 0) {
                remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
                if (remaining_timeout_us >= 0) {
                    continue;
                }
                errno = ETIMEDOUT;
            }
            if (errno != ETIMEDOUT) {
                connection->last_io_errno = errno;
                snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "send poll failed: %s", strerror(errno));
                return -1;
            }
            if (timed_out != NULL) {
                *timed_out = true;
            }
            connection->last_io_errno = errno;
            set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
            return -1;
        }
        connection->last_io_errno = errno;
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "send failed: %s", strerror(errno));
        return -1;
    }
}

static int h2_connection_write_all(h2_connection *connection, const uint8_t *data, size_t length, uint64_t deadline_abs_us, bool *timed_out)
{
    size_t total_written = 0;
    while (total_written < length) {
        ssize_t written = h2_connection_send(connection, data + total_written, length - total_written, deadline_abs_us, timed_out);
        if (written <= 0) {
            return FAILURE;
        }
        total_written += (size_t) written;
    }
    return SUCCESS;
}

static int h2_connection_flush_write_buffer(h2_connection *connection, uint64_t deadline_abs_us, bool *timed_out)
{
    if (connection == NULL || connection->write_buffer_len == 0) {
        return SUCCESS;
    }
    if (h2_connection_write_all(connection, connection->write_buffer, connection->write_buffer_len, deadline_abs_us, timed_out) != SUCCESS) {
        return FAILURE;
    }
    connection->write_buffer_len = 0;
    return SUCCESS;
}

static int h2_connection_buffer_or_write(h2_connection *connection, const uint8_t *data, size_t length, uint64_t deadline_abs_us, bool *timed_out)
{
    if (connection == NULL) {
        errno = EINVAL;
        return FAILURE;
    }
    if (!connection->write_coalescing) {
        return h2_connection_write_all(connection, data, length, deadline_abs_us, timed_out);
    }
    if (length > GRPC_LITE_H2_WRITE_COALESCE_CAPACITY) {
        if (h2_connection_flush_write_buffer(connection, deadline_abs_us, timed_out) != SUCCESS) {
            return FAILURE;
        }
        return h2_connection_write_all(connection, data, length, deadline_abs_us, timed_out);
    }
    if (connection->write_buffer == NULL) {
        connection->write_buffer = pemalloc(GRPC_LITE_H2_WRITE_COALESCE_CAPACITY, connection->persistent);
        if (connection->write_buffer == NULL) {
            errno = ENOMEM;
            connection->last_io_errno = errno;
            set_connection_error_detail(connection, "failed to allocate HTTP/2 write buffer");
            return FAILURE;
        }
        connection->write_buffer_cap = GRPC_LITE_H2_WRITE_COALESCE_CAPACITY;
    }
    if (connection->write_buffer_len + length > connection->write_buffer_cap) {
        if (h2_connection_flush_write_buffer(connection, deadline_abs_us, timed_out) != SUCCESS) {
            return FAILURE;
        }
    }
    memcpy(connection->write_buffer + connection->write_buffer_len, data, length);
    connection->write_buffer_len += length;
    return SUCCESS;
}

static ssize_t connection_recv(h2_connection *connection, uint8_t *data, size_t length, uint64_t deadline_abs_us)
{
    zend_long remaining_timeout_us;
    if (connection == NULL) {
        errno = EINVAL;
        return -1;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        errno = ETIMEDOUT;
        connection->last_io_errno = errno;
        set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
        return -1;
    }
    if (connection->ssl != NULL) {
        while (true) {
            int nread = SSL_read(connection->ssl, data, (int) length);
            if (nread > 0) {
                grpc_lite_trace_transport_io(connection, "wire.tls_read", length, nread, 0, 0);
                return nread;
            }
            int ssl_error = SSL_get_error(connection->ssl, nread);
            grpc_lite_trace_transport_io(connection, "wire.tls_read_retry", length, nread, ssl_error, 0);
            connection->last_ssl_error = ssl_error;
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                int poll_result = poll_fd_until_deadline(connection->fd, events, deadline_abs_us);
                if (poll_result == 0) {
                    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
                    if (remaining_timeout_us >= 0) {
                        continue;
                    }
                    errno = ETIMEDOUT;
                }
                if (errno != ETIMEDOUT) {
                    connection->last_io_errno = errno;
                    snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_read poll failed: %s", strerror(errno));
                    return -1;
                }
                connection->last_io_errno = errno;
                set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
                return -1;
            }
            errno = ECONNRESET;
            connection->last_io_errno = errno;
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_read failed: SSL_get_error=%d", ssl_error);
            return -1;
        }
    }
    while (true) {
        ssize_t nread = recv(connection->fd, data, length, 0);
        if (nread >= 0) {
            grpc_lite_trace_transport_io(connection, "wire.socket_read", length, nread, 0, 0);
            return nread;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int poll_result = poll_fd_until_deadline(connection->fd, POLLIN, deadline_abs_us);
            if (poll_result == 0) {
                remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
                if (remaining_timeout_us >= 0) {
                    continue;
                }
                errno = ETIMEDOUT;
            }
            if (errno != ETIMEDOUT) {
                connection->last_io_errno = errno;
                snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "recv poll failed: %s", strerror(errno));
                return -1;
            }
            connection->last_io_errno = errno;
            set_connection_error_detail(connection, "HTTP/2 transport deadline exceeded");
            return -1;
        }
        connection->last_io_errno = errno;
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "recv failed: %s", strerror(errno));
        return -1;
    }
}

static h2_connection *create_h2_connection(const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message)
{
    h2_connection *connection;
    int rv;
    uint32_t stream_window_size = effective_http2_window_size(PHP_GRPC_LITE_G(http2_stream_window_size));
    uint32_t connection_window_size = effective_http2_window_size(PHP_GRPC_LITE_G(http2_connection_window_size));

    connection = pecalloc(1, sizeof(h2_connection), persistent);
    connection->persistent = persistent;
    connection->fd = -1;
    connection->tls_verify_result = X509_V_OK;
    connection->fd = connect_tcp(host, port, deadline_abs_us);
    if (connection->fd < 0) {
        if (errno == ETIMEDOUT) {
            *error_message = "HTTP/2 transport deadline exceeded";
        } else {
            *error_message = "failed to connect";
        }
        pefree(connection, persistent);
        return NULL;
    }
    if (use_tls && configure_tls_connection(connection, host, tls_verify_name, tls_verify_name_len, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us) != 0) {
        if (connection->last_error_detail[0] != '\0') {
            snprintf(error_detail, error_detail_len, "%s", connection->last_error_detail);
            *error_message = error_detail;
        } else {
            *error_message = "failed to establish TLS";
        }
        destroy_h2_connection(connection);
        return NULL;
    }
    if (!use_tls && set_fd_nonblocking_mode(connection->fd, true) != 0) {
        destroy_h2_connection(connection);
        *error_message = "failed to set HTTP/2 socket nonblocking";
        return NULL;
    }

    connection->setup_deadline_abs_us = deadline_abs_us;

    if (configure_callbacks(&connection->callbacks) != 0) {
        destroy_h2_connection(connection);
        *error_message = "failed to configure callbacks";
        return NULL;
    }
    if (nghttp2_session_client_new(&connection->session, connection->callbacks, connection) != 0) {
        destroy_h2_connection(connection);
        *error_message = "failed to create nghttp2 session";
        return NULL;
    }
    {
        nghttp2_settings_entry settings[] = {
            { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
            { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, stream_window_size },
        };
        rv = nghttp2_submit_settings(connection->session, NGHTTP2_FLAG_NONE, settings, sizeof(settings) / sizeof(settings[0]));
        if (rv != 0) {
            destroy_h2_connection(connection);
            *error_message = "failed to submit HTTP/2 settings";
            return NULL;
        }
        if (connection_window_size > GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE) {
            rv = nghttp2_submit_window_update(connection->session, NGHTTP2_FLAG_NONE, 0, connection_window_size - GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE);
            if (rv != 0) {
                destroy_h2_connection(connection);
                *error_message = "failed to expand HTTP/2 connection receive window";
                return NULL;
            }
        }
    }
    rv = send_pending_h2_frames(connection, NULL);
    if (rv != 0) {
        if (connection->setup_timed_out) {
            *error_message = "HTTP/2 transport deadline exceeded";
        } else {
            *error_message = "nghttp2_session_send failed";
        }
        destroy_h2_connection(connection);
        return NULL;
    }
    build_authority(connection->authority, sizeof(connection->authority), host, port, authority, authority_len);
    return connection;
}

static h2_connection *get_persistent_connection(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message)
{
    persistent_connection_entry *entry;
    h2_connection *connection;

    if (persistent_reused != NULL) {
        *persistent_reused = false;
    }
    if (!PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        *error_message = "persistent connection cache is not initialized";
        return NULL;
    }

    entry = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_connections), key, key_len);
    connection = entry != NULL ? entry->connection : NULL;
    if (connection != NULL && !connection_usable(connection)) {
        remove_unusable_persistent_connection(key, key_len, connection);
        entry = NULL;
        connection = NULL;
    }
    if (connection != NULL && !preflight_persistent_connection(connection, deadline_abs_us)) {
        remove_unusable_persistent_connection(key, key_len, connection);
        entry = NULL;
        connection = NULL;
    }
    if (connection != NULL && !connection_entry_matches_key(entry, key, key_len)) {
        discard_persistent_connection(key, key_len, connection);
        entry = NULL;
        connection = NULL;
    }

    if (connection == NULL) {
        if (zend_hash_num_elements(&PHP_GRPC_LITE_G(persistent_connections)) >= GRPC_LITE_MAX_PERSISTENT_CONNECTIONS) {
            *error_message = "persistent connection cache limit exceeded";
            return NULL;
        }
        connection = create_h2_connection(host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, true, deadline_abs_us, error_detail, error_detail_len, error_message);
        if (connection == NULL) {
            return NULL;
        }
        entry = create_persistent_connection_entry(connection, key, key_len);
        if (entry == NULL) {
            destroy_h2_connection(connection);
            *error_message = "failed to allocate persistent connection entry";
            return NULL;
        }
        zend_hash_str_update_ptr(&PHP_GRPC_LITE_G(persistent_connections), key, key_len, entry);
        return connection;
    }

    if (persistent_reused != NULL) {
        *persistent_reused = true;
    }
    return connection;
}

static void discard_persistent_connection(const char *key, size_t key_len, h2_connection *connection)
{
    persistent_connection_entry *entry = NULL;

    if (PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        entry = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_connections), key, key_len);
        zend_hash_str_del(&PHP_GRPC_LITE_G(persistent_connections), key, key_len);
    }
    if (connection == NULL) {
        destroy_persistent_connection_entry(entry, false);
        return;
    }
    if (connection->stream_owner_count > 0) {
        connection->detached_from_cache = true;
        destroy_persistent_connection_entry(entry, false);
        return;
    }
    if (entry != NULL && entry->connection == connection) {
        destroy_persistent_connection_entry(entry, true);
        return;
    }
    destroy_h2_connection(connection);
}

static int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us)
{
    char port_buf[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_buf, sizeof(port_buf), "%ld", port);

    if (getaddrinfo(host, port_buf, &hints, &result) != 0) {
        return -1;
    }
    if (deadline_abs_us > 0 && monotonic_us() >= deadline_abs_us) {
        freeaddrinfo(result);
        errno = ETIMEDOUT;
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        int socket_error = 0;
        socklen_t socket_error_len = sizeof(socket_error);
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        if (set_fd_nonblocking_mode(fd, true) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            set_fd_nonblocking_mode(fd, false);
            break;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            if (poll_fd_until_deadline(fd, POLLOUT, deadline_abs_us) == 0
                && getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) == 0
                && socket_error == 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                set_fd_nonblocking_mode(fd, false);
                break;
            }
            if (socket_error != 0) {
                errno = socket_error;
            }
        }
        if (deadline_abs_us > 0 && monotonic_us() >= deadline_abs_us) {
            close(fd);
            fd = -1;
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    uint64_t deadline_abs_us;
    bool *timed_out;
    (void) session;
    (void) flags;

    if (connection == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    timed_out = connection->current_write_deadline_abs_us > 0
        ? &connection->current_write_timed_out
        : &connection->setup_timed_out;
    deadline_abs_us = connection->current_write_deadline_abs_us > 0
        ? connection->current_write_deadline_abs_us
        : connection->setup_deadline_abs_us;
    if (h2_connection_buffer_or_write(connection, data, length, deadline_abs_us, timed_out) != SUCCESS) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    grpc_lite_trace_outbound_frame(connection, data, length);
    return (ssize_t) length;
}

static int send_pending_h2_frames_with_deadline(h2_connection *connection, grpc_call *call, uint64_t fallback_deadline_abs_us)
{
    int rv;

    if (connection == NULL || connection->session == NULL) {
        return NGHTTP2_ERR_INVALID_ARGUMENT;
    }
    connection->current_io_call = call;
    connection->current_write_deadline_abs_us = call != NULL ? call->deadline_abs_us : fallback_deadline_abs_us;
    connection->current_write_timed_out = false;
    connection->write_coalescing = connection->tls;
    connection->write_buffer_len = 0;
    rv = nghttp2_session_send(connection->session);
    if (rv == 0) {
        bool *timed_out = connection->current_write_deadline_abs_us > 0
            ? &connection->current_write_timed_out
            : &connection->setup_timed_out;
        uint64_t deadline_abs_us = connection->current_write_deadline_abs_us > 0
            ? connection->current_write_deadline_abs_us
            : connection->setup_deadline_abs_us;
        if (h2_connection_flush_write_buffer(connection, deadline_abs_us, timed_out) != SUCCESS) {
            rv = NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }
    if (rv != 0 && call != NULL) {
        if (connection->current_write_timed_out) {
            call->timed_out = true;
        }
        call->last_io_errno = connection->last_io_errno;
        call->last_ssl_error = connection->last_ssl_error;
        snprintf(call->last_io_error_detail, sizeof(call->last_io_error_detail), "%s", connection->last_error_detail);
    }
    connection->current_io_call = NULL;
    connection->current_write_deadline_abs_us = 0;
    connection->current_write_timed_out = false;
    connection->write_coalescing = false;
    connection->write_buffer_len = 0;
    return rv;
}

static int send_pending_h2_frames(h2_connection *connection, grpc_call *call)
{
    return send_pending_h2_frames_with_deadline(
        connection,
        call,
        connection != NULL ? connection->setup_deadline_abs_us : 0
    );
}

static size_t remaining_request_bytes(grpc_call *call)
{
    size_t total_len = call->grpc_header_len + call->request_len;
    if (call->request_offset >= total_len) {
        return 0;
    }
    return total_len - call->request_offset;
}

static size_t copy_request_bytes(grpc_call *call, uint8_t *buf, size_t length)
{
    size_t copied = 0;
    size_t total_len = call->grpc_header_len + call->request_len;

    while (copied < length && call->request_offset < total_len) {
        if (call->request_offset < call->grpc_header_len) {
            size_t header_offset = call->request_offset;
            size_t remaining = call->grpc_header_len - header_offset;
            size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
            memcpy(buf + copied, call->grpc_header + header_offset, to_copy);
            copied += to_copy;
            call->request_offset += to_copy;
            continue;
        }

        size_t payload_offset = call->request_offset - call->grpc_header_len;
        size_t remaining = call->request_len - payload_offset;
        size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
        memcpy(buf + copied, call->request + payload_offset, to_copy);
        copied += to_copy;
        call->request_offset += to_copy;
    }

    return copied;
}

static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    grpc_call *call = source != NULL ? (grpc_call *) source->ptr : NULL;
    size_t total_len;
    size_t remaining;
    size_t to_send;
    (void) session;
    (void) stream_id;
    (void) user_data;

    *data_flags = 0;
    if (call == NULL) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    total_len = call->grpc_header_len + call->request_len;
    remaining = remaining_request_bytes(call);
    to_send = remaining < length ? remaining : length;

    size_t copied = copy_request_bytes(call, buf, to_send);
    if (call->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static void grpc_protocol_set_message_header(grpc_call *call, size_t payload_len)
{
    call->grpc_header[0] = 0;
    call->grpc_header[1] = (uint8_t) ((payload_len >> 24) & 0xff);
    call->grpc_header[2] = (uint8_t) ((payload_len >> 16) & 0xff);
    call->grpc_header[3] = (uint8_t) ((payload_len >> 8) & 0xff);
    call->grpc_header[4] = (uint8_t) (payload_len & 0xff);
    call->grpc_header_len = 5;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    grpc_call *call = grpc_call_from_stream_id(connection, stream_id);
    (void) session;
    (void) flags;
    if (call == NULL) {
        return 0;
    }
    if (stream_id == call->stream_id && len > 0) {
        if (call->direct_response_payload && call->decode_response_incrementally && call->queue_response_payloads) {
            if (grpc_protocol_process_response_data_direct(session, call, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        } else {
            if (grpc_protocol_validate_response_message_lengths(session, call, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (call->discard_response_body) {
                return 0;
            }
            smart_str_appendl(&call->body, (const char *) data, len);
        }
    }
    return 0;
}

static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    grpc_call *call = grpc_call_from_stream_id(connection, frame->hd.stream_id);
    bool trailing;
    (void) session;
    (void) flags;
    if (call == NULL) {
        return 0;
    }
    if (frame->hd.stream_id != call->stream_id) {
        return 0;
    }
    trailing = frame->headers.cat != NGHTTP2_HCAT_RESPONSE;
    if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE && call->grpc_status >= 0) {
        trailing = true;
    }
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            call->initial_grpc_status_seen = true;
        }
        if (call->grpc_status_seen) {
            call->invalid_grpc_status = true;
        }
        call->grpc_status_seen = true;
        call->grpc_status = grpc_protocol_parse_status_value(value, valuelen);
        if (call->grpc_status < 0) {
            call->invalid_grpc_status = true;
        }
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            grpc_protocol_mark_response_metadata_as_trailing(call);
        }
        trailing = true;
    } else if (namelen == sizeof("grpc-message") - 1 && memcmp(name, "grpc-message", namelen) == 0) {
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            call->initial_grpc_status_seen = true;
        }
        if (call->grpc_message != NULL) {
            zend_string_release(call->grpc_message);
        }
        call->grpc_message = grpc_protocol_decode_message(value, valuelen);
        trailing = true;
    } else if (namelen == sizeof("grpc-status-details-bin") - 1 && memcmp(name, "grpc-status-details-bin", namelen) == 0) {
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            call->initial_grpc_status_seen = true;
        }
        trailing = true;
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        call->http_status = atoi(status_buf);
    } else if (namelen == sizeof("content-type") - 1 && memcmp(name, "content-type", namelen) == 0) {
        call->content_type_seen = true;
        if (call->content_type != NULL) {
            zend_string_release(call->content_type);
        }
        call->content_type = zend_string_init((const char *) value, valuelen, 0);
        if (!grpc_protocol_is_valid_content_type(value, valuelen)) {
            call->invalid_content_type = true;
            call->discard_response_body = true;
        }
    } else if (namelen == sizeof("grpc-encoding") - 1 && memcmp(name, "grpc-encoding", namelen) == 0) {
        if (call->grpc_encoding != NULL) {
            zend_string_release(call->grpc_encoding);
        }
        call->grpc_encoding = zend_string_init((const char *) value, valuelen, 0);
        if (!grpc_protocol_is_identity_encoding(value, valuelen)) {
            call->unsupported_response_encoding = true;
            call->discard_response_body = true;
        }
    }
    if (grpc_protocol_add_response_metadata_entry(call, name, namelen, value, valuelen, trailing) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    grpc_call *call = grpc_call_from_stream_id(connection, stream_id);
    (void) session;
    (void) error_code;
    if (call == NULL) {
        return 0;
    }
    if (stream_id == call->stream_id) {
        call->stream_closed = true;
        call->stream_error_code = error_code;
        unregister_grpc_call_stream(call);
    }
    return 0;
}

static int on_begin_frame_callback(nghttp2_session *session, const nghttp2_frame_hd *hd, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    (void) session;

    grpc_lite_trace_inbound_frame_header(connection, hd);
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    grpc_call *call = grpc_call_from_stream_id(connection, frame->hd.stream_id);
    (void) session;

    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) {
        grpc_lite_trace_inbound_frame(connection, frame);
    }

    if (frame->hd.type == NGHTTP2_GOAWAY) {
        if (connection != NULL) {
            grpc_call *active_stream = connection->active_streams;
            mark_connection_draining(connection, frame->goaway.last_stream_id, frame->goaway.error_code);
            while (active_stream != NULL) {
                grpc_call *next_active_stream = active_stream->next_active_stream;
                if (active_stream->stream_id > 0 && frame->goaway.last_stream_id < active_stream->stream_id) {
                    active_stream->stream_error_code = NGHTTP2_REFUSED_STREAM;
                    active_stream->stream_refused_seen = true;
                    active_stream->stream_closed = true;
                    unregister_grpc_call_stream(active_stream);
                }
                active_stream = next_active_stream;
            }
        }
        return 0;
    }
    if (call == NULL) {
        return 0;
    }
    if (frame->hd.type == NGHTTP2_HEADERS
        && frame->hd.stream_id == call->stream_id
        && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        call->initial_headers_end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
        if (!call->content_type_seen && !(call->grpc_status_seen && call->initial_headers_end_stream)) {
            call->invalid_content_type = true;
            call->discard_response_body = true;
        }
        if (call->initial_grpc_status_seen && !call->initial_headers_end_stream) {
            call->invalid_grpc_status = true;
            call->discard_response_body = true;
        }
    } else if (frame->hd.type == NGHTTP2_RST_STREAM && frame->hd.stream_id == call->stream_id) {
        call->stream_reset_seen = true;
        call->stream_error_code = frame->rst_stream.error_code;
    } else if (frame->hd.type == NGHTTP2_PUSH_PROMISE && frame->hd.stream_id == call->stream_id) {
        call->malformed_response_frame = true;
        call->discard_response_body = true;
        if (session != NULL && call->stream_id > 0) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_PROTOCOL_ERROR);
        }
    }
    return 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000ULL) + ((uint64_t) ts.tv_nsec / 1000ULL);
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen)
{
    char buf[32];
    size_t copy_len = valuelen < sizeof(buf) - 1 ? valuelen : sizeof(buf) - 1;
    memcpy(buf, value, copy_len);
    buf[copy_len] = '\0';
    return (zend_long) atoll(buf);
}
#endif

static zend_string *grpc_protocol_decode_message(const uint8_t *value, size_t valuelen)
{
    smart_str decoded = {0};
    size_t index = 0;

    while (index < valuelen) {
        if (value[index] == '%' && index + 2 < valuelen) {
            int high = grpc_lite_hex_value(value[index + 1]);
            int low = grpc_lite_hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                smart_str_appendc(&decoded, (char) ((high << 4) | low));
                index += 3;
                continue;
            }
        }
        smart_str_appendc(&decoded, (char) value[index]);
        index++;
    }
    smart_str_0(&decoded);
    return decoded.s != NULL ? decoded.s : zend_string_copy(zend_empty_string);
}

static zend_string *grpc_lite_status_details_from_call(grpc_call *call, int code)
{
    if (call->grpc_message != NULL && ZSTR_LEN(call->grpc_message) > 0) {
        return zend_string_copy(call->grpc_message);
    }
    if (call->stream_refused_seen) {
        return zend_string_init("HTTP/2 stream refused by GOAWAY", sizeof("HTTP/2 stream refused by GOAWAY") - 1, 0);
    }
    if (call->last_io_error_detail[0] != '\0') {
        return zend_string_init(call->last_io_error_detail, strlen(call->last_io_error_detail), 0);
    }
    if (call->connection != NULL && call->connection->last_error_detail[0] != '\0') {
        return zend_string_init(call->connection->last_error_detail, strlen(call->connection->last_error_detail), 0);
    }
    if (call->http_status != 200) {
        return strpprintf(0, "HTTP status %d without grpc-status", call->http_status);
    }
    if (call->invalid_content_type) {
        if (call->content_type != NULL && ZSTR_LEN(call->content_type) > 0) {
            return strpprintf(0, "invalid gRPC content-type: %s", ZSTR_VAL(call->content_type));
        }
        return zend_string_init("invalid gRPC content-type", sizeof("invalid gRPC content-type") - 1, 0);
    }
    if (call->invalid_grpc_status) {
        return zend_string_init("invalid grpc-status trailer", sizeof("invalid grpc-status trailer") - 1, 0);
    }
    switch (code) {
        case GRPC_STATUS_DEADLINE_EXCEEDED:
            return zend_string_init("HTTP/2 transport deadline exceeded", sizeof("HTTP/2 transport deadline exceeded") - 1, 0);
        case GRPC_STATUS_UNAVAILABLE:
            if (call->stream_reset_seen) {
                return strpprintf(0, "HTTP/2 stream reset: %u", call->stream_error_code);
            }
            return zend_string_copy(zend_empty_string);
        case GRPC_STATUS_RESOURCE_EXHAUSTED:
            if (call->response_queue_limit_exceeded) {
                return zend_string_init("server streaming read-ahead queue limit exceeded", sizeof("server streaming read-ahead queue limit exceeded") - 1, 0);
            }
            return zend_string_init("received message exceeds maximum size", sizeof("received message exceeds maximum size") - 1, 0);
        case GRPC_STATUS_INTERNAL:
            if (call->malformed_response_frame) {
                return zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
            }
            if (call->stream_reset_seen) {
                return strpprintf(0, "HTTP/2 stream reset: %u", call->stream_error_code);
            }
            return zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
        case GRPC_STATUS_UNIMPLEMENTED:
            if (call->unsupported_response_encoding) {
                if (call->grpc_encoding != NULL && ZSTR_LEN(call->grpc_encoding) > 0) {
                    return strpprintf(0, "unsupported grpc-encoding: %s", ZSTR_VAL(call->grpc_encoding));
                }
                return zend_string_init("unsupported grpc-encoding", sizeof("unsupported grpc-encoding") - 1, 0);
            }
            return zend_string_init("compressed gRPC messages are not supported", sizeof("compressed gRPC messages are not supported") - 1, 0);
        case GRPC_STATUS_CANCELLED:
            return zend_string_init("Cancelled", sizeof("Cancelled") - 1, 0);
        default:
            return zend_string_copy(zend_empty_string);
    }
}

static void resolve_grpc_call_status(grpc_call *call, bool cancelled, grpc_lite_status_result *result)
{
    result->code = grpc_lite_status_code_from_call(call, cancelled);
    result->details = grpc_lite_status_details_from_call(call, result->code);
}

static void add_status_result_to_return(zval *return_value, grpc_lite_status_result *status)
{
    add_assoc_long(return_value, "status_code", status->code);
    add_assoc_str(return_value, "status_details", status->details != NULL ? zend_string_copy(status->details) : zend_empty_string);
}

static int init_request_headers(h2_request_headers *headers)
{
    headers->capacity = GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY;
    headers->len = 0;
    headers->name_count = 0;
    headers->value_count = 0;
    headers->custom_value_count = 0;
    headers->nva = headers->inline_nva;
    headers->name_strings = headers->inline_name_strings;
    headers->value_strings = headers->inline_value_strings;
    return 0;
}

static void grow_request_headers(h2_request_headers *headers)
{
    size_t new_capacity = headers->capacity * 2;
    nghttp2_nv *new_nva;
    zend_string **new_name_strings;
    zend_string **new_value_strings;

    if (new_capacity < headers->capacity || new_capacity > GRPC_LITE_MAX_REQUEST_METADATA_VALUES + 7) {
        new_capacity = GRPC_LITE_MAX_REQUEST_METADATA_VALUES + 7;
    }
    if (new_capacity <= headers->capacity) {
        return;
    }

    new_nva = ecalloc(new_capacity, sizeof(nghttp2_nv));
    new_name_strings = ecalloc(new_capacity, sizeof(zend_string *));
    new_value_strings = ecalloc(new_capacity, sizeof(zend_string *));

    memcpy(new_nva, headers->nva, headers->len * sizeof(nghttp2_nv));
    memcpy(new_name_strings, headers->name_strings, headers->name_count * sizeof(zend_string *));
    memcpy(new_value_strings, headers->value_strings, headers->value_count * sizeof(zend_string *));

    if (headers->nva != headers->inline_nva) {
        efree(headers->nva);
    }
    if (headers->name_strings != headers->inline_name_strings) {
        efree(headers->name_strings);
    }
    if (headers->value_strings != headers->inline_value_strings) {
        efree(headers->value_strings);
    }

    headers->nva = new_nva;
    headers->name_strings = new_name_strings;
    headers->value_strings = new_value_strings;
    headers->capacity = new_capacity;
}

static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen)
{
    if (headers->len >= headers->capacity) {
        grow_request_headers(headers);
        if (headers->len >= headers->capacity) {
            return;
        }
    }
    headers->nva[headers->len++] = (nghttp2_nv) {
        (uint8_t *) name,
        (uint8_t *) value,
        namelen,
        valuelen,
        NGHTTP2_NV_FLAG_NONE
    };
}

static void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us)
{
    char timeout_buf[32];
    size_t timeout_len;
    zend_string *value_str;
    timeout_len = grpc_lite_format_timeout_us(timeout_buf, sizeof(timeout_buf), (long) timeout_us);
    if (timeout_len == 0) {
        return;
    }
    value_str = zend_string_init(timeout_buf, timeout_len, 0);
    if (headers->value_strings != NULL && headers->value_count < headers->capacity) {
        headers->value_strings[headers->value_count++] = value_str;
    } else {
        zend_string_release(value_str);
        return;
    }
    append_request_header(headers, "grpc-timeout", sizeof("grpc-timeout") - 1, ZSTR_VAL(value_str), ZSTR_LEN(value_str));
}

static void append_user_agent_request_header(h2_request_headers *headers, zend_string *primary_user_agent)
{
    if (primary_user_agent != NULL && ZSTR_LEN(primary_user_agent) > 0) {
        append_request_header(headers, "user-agent", sizeof("user-agent") - 1, ZSTR_VAL(primary_user_agent), ZSTR_LEN(primary_user_agent));
        return;
    }
    append_request_header(headers, "user-agent", sizeof("user-agent") - 1, "php-grpc-lite/0.1.0", sizeof("php-grpc-lite/0.1.0") - 1);
}

static bool is_valid_custom_request_header_name_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '_' || ch == '.' || ch == '-';
}

static bool is_reserved_custom_request_header(const char *name, size_t name_len)
{
    static const char *reserved_headers[] = {
        "content-type",
        "te",
        "grpc-accept-encoding",
        "grpc-encoding",
        "grpc-message",
        "grpc-status",
        "grpc-status-details-bin",
        "host",
        "connection",
        "keep-alive",
        "proxy-connection",
        "transfer-encoding",
        "upgrade",
    };
    size_t index;

    for (index = 0; index < sizeof(reserved_headers) / sizeof(reserved_headers[0]); index++) {
        if (strlen(reserved_headers[index]) == name_len && memcmp(name, reserved_headers[index], name_len) == 0) {
            return true;
        }
    }
    if (name_len > sizeof("grpc-") - 1 && memcmp(name, "grpc-", sizeof("grpc-") - 1) == 0) return true;
    return false;
}

static bool is_forbidden_custom_request_header(zend_string *key)
{
    const char *name;
    size_t name_len;
    size_t index;

    if (key == NULL || ZSTR_LEN(key) == 0) {
        return true;
    }
    name = ZSTR_VAL(key);
    name_len = ZSTR_LEN(key);
    if (name[0] == ':') {
        return true;
    }
    for (index = 0; index < name_len; index++) {
        if (!is_valid_custom_request_header_name_char((unsigned char) name[index])) {
            return true;
        }
    }
    if (is_reserved_custom_request_header(name, name_len)) {
        return true;
    }
    return false;
}

static bool is_user_agent_custom_request_header(zend_string *key)
{
    return key != NULL
        && ZSTR_LEN(key) == sizeof("user-agent") - 1
        && memcmp(ZSTR_VAL(key), "user-agent", sizeof("user-agent") - 1) == 0;
}

static bool is_binary_metadata_header(zend_string *key)
{
    return key != NULL
        && ZSTR_LEN(key) >= sizeof("-bin") - 1
        && memcmp(ZSTR_VAL(key) + ZSTR_LEN(key) - (sizeof("-bin") - 1), "-bin", sizeof("-bin") - 1) == 0;
}

static bool is_invalid_binary_request_header_value(zend_string *value)
{
    const char *bytes;
    size_t length;
    size_t index;

    if (value == NULL) {
        return true;
    }
    bytes = ZSTR_VAL(value);
    length = ZSTR_LEN(value);
    for (index = 0; index < length; index++) {
        unsigned char ch = (unsigned char) bytes[index];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=')) {
            return true;
        }
    }
    return false;
}

static bool is_invalid_ascii_request_header_value(zend_string *value)
{
    const char *bytes;
    size_t length;
    size_t index;

    if (value == NULL) {
        return true;
    }
    bytes = ZSTR_VAL(value);
    length = ZSTR_LEN(value);
    for (index = 0; index < length; index++) {
        unsigned char ch = (unsigned char) bytes[index];
        if (ch < 0x20 || ch == 0x7f || ch >= 0x80) {
            return true;
        }
    }
    return false;
}

static int append_custom_request_header_value(h2_request_headers *headers, zend_string *key, zval *value)
{
    zend_string *name_str = NULL;
    zend_string *value_str;

    if (Z_TYPE_P(value) != IS_STRING) {
        zend_throw_exception(NULL, "gRPC request metadata value must be a string", 0);
        return -1;
    }
    if (headers->custom_value_count >= GRPC_LITE_MAX_REQUEST_METADATA_VALUES) {
        zend_throw_exception(NULL, "gRPC request metadata exceeds maximum count", 0);
        return -1;
    }
    if (headers->len >= headers->capacity || headers->name_count >= headers->capacity || headers->value_count >= headers->capacity) {
        grow_request_headers(headers);
    }
    if (headers->name_strings == NULL || headers->value_strings == NULL || headers->len >= headers->capacity || headers->name_count >= headers->capacity || headers->value_count >= headers->capacity) {
        zend_throw_exception(NULL, "gRPC request metadata exceeds maximum count", 0);
        return -1;
    }
    name_str = zend_string_copy(key);
    if (is_binary_metadata_header(name_str)) {
        value_str = php_base64_encode((const unsigned char *) Z_STRVAL_P(value), Z_STRLEN_P(value));
    } else {
        value_str = zend_string_copy(Z_STR_P(value));
    }
    if (is_binary_metadata_header(name_str) ? is_invalid_binary_request_header_value(value_str) : is_invalid_ascii_request_header_value(value_str)) {
        zend_string_release(name_str);
        zend_string_release(value_str);
        zend_throw_exception(NULL, "invalid gRPC request metadata value", 0);
        return -1;
    }
    if (headers->name_strings != NULL) {
        headers->name_strings[headers->name_count++] = name_str;
    }
    if (headers->value_strings != NULL) {
        headers->value_strings[headers->value_count++] = value_str;
    }
    append_request_header(headers, ZSTR_VAL(name_str), ZSTR_LEN(name_str), ZSTR_VAL(value_str), ZSTR_LEN(value_str));
    headers->custom_value_count++;
    return 0;
}

static int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv)
{
    zend_string *key;
    zval *value;

    if (headers_zv == NULL || Z_TYPE_P(headers_zv) != IS_ARRAY) {
        return 0;
    }

    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
        if (is_user_agent_custom_request_header(key)) {
            continue;
        }
        if (is_forbidden_custom_request_header(key)) {
            zend_throw_exception(NULL, "forbidden gRPC request metadata key", 0);
            return -1;
        }
        if (Z_TYPE_P(value) == IS_ARRAY) {
            zval *nested;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), nested) {
                if (append_custom_request_header_value(headers, key, nested) != 0) {
                    return -1;
                }
            } ZEND_HASH_FOREACH_END();
            continue;
        }

        if (append_custom_request_header_value(headers, key, value) != 0) {
            return -1;
        }
    } ZEND_HASH_FOREACH_END();

    return 0;
}

static void free_request_headers(h2_request_headers *headers)
{
    size_t i;
    if (headers->name_strings != NULL) {
        for (i = 0; i < headers->name_count; i++) {
            if (headers->name_strings[i] != NULL) {
                zend_string_release(headers->name_strings[i]);
            }
        }
        if (headers->name_strings != headers->inline_name_strings) {
            efree(headers->name_strings);
        }
    }
    if (headers->value_strings != NULL) {
        for (i = 0; i < headers->value_count; i++) {
            if (headers->value_strings[i] != NULL) {
                zend_string_release(headers->value_strings[i]);
            }
        }
        if (headers->value_strings != headers->inline_value_strings) {
            efree(headers->value_strings);
        }
    }
    if (headers->nva != NULL && headers->nva != headers->inline_nva) {
        efree(headers->nva);
    }
    headers->nva = NULL;
    headers->name_strings = NULL;
    headers->value_strings = NULL;
    headers->len = 0;
    headers->capacity = 0;
    headers->name_count = 0;
    headers->value_count = 0;
    headers->custom_value_count = 0;
}

static int grpc_protocol_validate_response_message_lengths(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    if (call->grpc_status_seen) {
        call->invalid_grpc_status = true;
        call->discard_response_body = true;
        if (session != NULL && call->stream_id > 0) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
        }
        return 0;
    }

    while (offset < len && !call->response_message_too_large) {
        if (call->response_header_len < 5) {
            size_t need = 5 - call->response_header_len;
            size_t take = need < len - offset ? need : len - offset;
            memcpy(call->response_header_buf + call->response_header_len, data + offset, take);
            call->response_header_len += take;
            offset += take;
            if (call->response_header_len < 5) {
                continue;
            }

            call->response_payload_len = ((uint32_t) call->response_header_buf[1] << 24)
                | ((uint32_t) call->response_header_buf[2] << 16)
                | ((uint32_t) call->response_header_buf[3] << 8)
                | (uint32_t) call->response_header_buf[4];
            if (call->max_response_messages > 0 && ++call->response_message_count > call->max_response_messages) {
                call->malformed_response_frame = true;
                call->discard_response_body = true;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
            if (call->response_header_buf[0] > 1) {
                call->malformed_response_frame = true;
                call->discard_response_body = true;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
            if (call->response_header_buf[0] == 1) {
                call->compressed_response_seen = true;
                call->discard_response_body = true;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
            call->response_payload_offset = 0;
            if ((size_t) call->response_payload_len > call->max_receive_message_bytes) {
                call->response_message_too_large = true;
                call->discard_response_body = true;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
            if (call->response_payload_len == 0) {
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                continue;
            }
        }

        if (call->response_payload_len > 0) {
            size_t need = call->response_payload_len - call->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            call->response_payload_offset += take;
            offset += take;
            if (call->response_payload_offset == call->response_payload_len) {
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
            }
        }
    }

    return 0;
}

static int grpc_protocol_process_response_data_direct(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    if (call->grpc_status_seen) {
        call->invalid_grpc_status = true;
        call->discard_response_body = true;
        if (session != NULL && call->stream_id > 0) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
        }
        return 0;
    }

    while (offset < len && !call->response_message_too_large && !call->compressed_response_seen && !call->malformed_response_frame) {
        if (call->response_header_len < 5) {
            size_t need = 5 - call->response_header_len;
            size_t take = need < len - offset ? need : len - offset;
            memcpy(call->response_header_buf + call->response_header_len, data + offset, take);
            call->response_header_len += take;
            offset += take;
            if (call->response_header_len < 5) {
                continue;
            }

            call->response_payload_len = ((uint32_t) call->response_header_buf[1] << 24)
                | ((uint32_t) call->response_header_buf[2] << 16)
                | ((uint32_t) call->response_header_buf[3] << 8)
                | (uint32_t) call->response_header_buf[4];
            if (call->max_response_messages > 0 && ++call->response_message_count > call->max_response_messages) {
                call->malformed_response_frame = true;
                call->discard_response_body = true;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                continue;
            }
            if ((size_t) call->response_payload_len > call->max_receive_message_bytes) {
                call->response_message_too_large = true;
                call->discard_response_body = true;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                call->response_current_compressed = false;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                call->response_payload_offset = 0;
                continue;
            }
            call->response_current_compressed = call->response_header_buf[0] != 0;
            if (call->response_header_buf[0] > 1) {
                call->malformed_response_frame = true;
                call->discard_response_body = true;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                call->response_current_compressed = false;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                continue;
            }
            if (call->response_header_buf[0] == 1) {
                call->compressed_response_seen = true;
                call->discard_response_body = true;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                call->response_current_compressed = false;
                if (session != NULL && call->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
                }
                continue;
            }
            if (server_streaming_read_ahead_limit_would_exceed(call, call->response_payload_len)) {
                mark_server_streaming_read_ahead_limit_exceeded(session, call);
                return 0;
            }
            call->response_payload_offset = 0;
            if (call->response_current_compressed) {
                if (call->response_payload_len == 0) {
                    call->response_header_len = 0;
                    call->response_payload_len = 0;
                    call->response_payload_offset = 0;
                    call->response_current_compressed = false;
                }
            } else {
                call->response_payload = zend_string_alloc(call->response_payload_len, 0);
                if (call->response_payload_len == 0) {
                    ZSTR_VAL(call->response_payload)[0] = '\0';
                }
            }
        }

        if (call->response_current_compressed) {
            size_t need = call->response_payload_len - call->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            call->response_payload_offset += take;
            offset += take;
            if (call->response_payload_offset == call->response_payload_len) {
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;
                call->response_current_compressed = false;
            }
        } else if (call->response_payload != NULL) {
            size_t need = call->response_payload_len - call->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            if (take > 0) {
                memcpy(ZSTR_VAL(call->response_payload) + call->response_payload_offset, data + offset, take);
                call->response_payload_offset += take;
                offset += take;
            }

            if (call->response_payload_offset == call->response_payload_len) {
                zend_string *payload = call->response_payload;
                ZSTR_VAL(payload)[call->response_payload_len] = '\0';
                call->response_payload = NULL;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;

                if (call->queue_response_payloads) {
                    if (enqueue_response_payload(session, call, payload) != 0) {
                        return -1;
                    }
                } else {
                    zend_string_release(payload);
                }
            }
        }
    }

    return 0;
}

static bool server_streaming_read_ahead_limit_would_exceed(grpc_call *call, size_t payload_len)
{
    size_t max_messages = effective_server_streaming_read_ahead_max_messages();
    size_t max_bytes = effective_server_streaming_read_ahead_max_bytes();
    return call != NULL
        && call->queue_response_payloads
        && call->connection != NULL
        && call->connection->current_read_call != NULL
        && call->connection->current_read_call != call
        && (call->response_queue_count >= max_messages
            || payload_len > max_bytes
            || call->response_queue_bytes > max_bytes - payload_len);
}

static void mark_server_streaming_read_ahead_limit_exceeded(nghttp2_session *session, grpc_call *call)
{
    if (call == NULL) {
        return;
    }
    call->response_queue_limit_exceeded = true;
    call->discard_response_body = true;
    call->response_header_len = 0;
    call->response_payload_len = 0;
    call->response_payload_offset = 0;
    call->response_current_compressed = false;
    if (session != NULL && call->stream_id > 0) {
        nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
    }
}

static int enqueue_response_payload(nghttp2_session *session, grpc_call *call, zend_string *payload)
{
    if (server_streaming_read_ahead_limit_would_exceed(call, ZSTR_LEN(payload))) {
        zend_string_release(payload);
        mark_server_streaming_read_ahead_limit_exceeded(session, call);
        return 0;
    }

    queued_payload *entry = emalloc(sizeof(queued_payload));
    entry->payload = payload;
    entry->next = NULL;

    if (call->response_queue_tail == NULL) {
        call->response_queue_head = entry;
    } else {
        call->response_queue_tail->next = entry;
    }
    call->response_queue_tail = entry;
    call->response_queue_count++;
    call->response_queue_bytes += ZSTR_LEN(payload);

    return 0;
}

static void free_queued_response_payloads(grpc_call *call)
{
    while (call->response_queue_head != NULL) {
        queued_payload *entry = call->response_queue_head;
        call->response_queue_head = entry->next;
        zend_string_release(entry->payload);
        efree(entry);
    }
    call->response_queue_tail = NULL;
    call->response_queue_count = 0;
    call->response_queue_bytes = 0;
}

static int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing)
{
    metadata_entry *entry;
    size_t entry_bytes = namelen + valuelen;

    if (namelen == 0 || name[0] == ':') {
        return 0;
    }
    if (call->metadata_too_large
        || call->metadata_entry_count >= GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES
        || entry_bytes > call->max_response_metadata_bytes
        || call->metadata_bytes > call->max_response_metadata_bytes - entry_bytes) {
        call->metadata_too_large = true;
        call->discard_response_body = true;
        if (call->stream_id > 0) {
            nghttp2_session *session = call->connection != NULL ? call->connection->session : NULL;
            if (session != NULL) {
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
            }
        }
        return 0;
    }

    entry = emalloc(sizeof(metadata_entry));
    entry->key = zend_string_init((const char *) name, namelen, 0);
    entry->value = zend_string_init((const char *) value, valuelen, 0);
    entry->trailing = trailing;
    entry->next = NULL;

    if (call->metadata_tail == NULL) {
        call->metadata_head = entry;
    } else {
        call->metadata_tail->next = entry;
    }
    call->metadata_tail = entry;
    call->metadata_entry_count++;
    call->metadata_bytes += entry_bytes;

    return 0;
}

static void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call)
{
    metadata_entry *entry;
    for (entry = call->metadata_head; entry != NULL; entry = entry->next) {
        entry->trailing = true;
    }
}

static void grpc_protocol_free_response_metadata_entries(grpc_call *call)
{
    while (call->metadata_head != NULL) {
        metadata_entry *entry = call->metadata_head;
        call->metadata_head = entry->next;
        zend_string_release(entry->key);
        zend_string_release(entry->value);
        efree(entry);
    }
    call->metadata_tail = NULL;
}

static void add_metadata_value_to_map(zval *metadata, zend_string *key, zend_string *value)
{
    zval *values = zend_hash_find(Z_ARRVAL_P(metadata), key);
    if (values == NULL) {
        zval new_values;
        array_init(&new_values);
        add_next_index_str(&new_values, value);
        zend_hash_update(Z_ARRVAL_P(metadata), key, &new_values);
    } else if (Z_TYPE_P(values) == IS_ARRAY) {
        add_next_index_str(values, value);
    } else {
        zend_string_release(value);
    }
}

static void add_binary_metadata_values_to_map(zval *metadata, zend_string *key, zend_string *wire_value)
{
    const char *bytes = ZSTR_VAL(wire_value);
    size_t length = ZSTR_LEN(wire_value);
    size_t start = 0;

    while (start <= length) {
        size_t end = start;
        zend_string *value;

        while (end < length && bytes[end] != ',') {
            end++;
        }
        value = php_base64_decode((const unsigned char *) bytes + start, end - start);
        if (value == NULL) {
            value = zend_string_init(bytes + start, end - start, 0);
        }
        add_metadata_value_to_map(metadata, key, value);
        if (end == length) {
            break;
        }
        start = end + 1;
    }
}

static void grpc_protocol_copy_metadata_map(zval *metadata, grpc_call *call, bool trailing)
{
    metadata_entry *entry;

    array_init(metadata);
    for (entry = call->metadata_head; entry != NULL; entry = entry->next) {
        if (entry->trailing != trailing) {
            continue;
        }
        if (is_binary_metadata_header(entry->key)) {
            add_binary_metadata_values_to_map(metadata, entry->key, entry->value);
        } else {
            add_metadata_value_to_map(metadata, entry->key, zend_string_copy(entry->value));
        }
    }
}

static void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing)
{
    zval metadata;

    grpc_protocol_copy_metadata_map(&metadata, call, trailing);
    add_assoc_zval(return_value, name, &metadata);
}

static void cleanup_grpc_call(grpc_call *call)
{
    if (call == NULL) {
        return;
    }
    if (call->response_payload != NULL) {
        zend_string_release(call->response_payload);
        call->response_payload = NULL;
    }
    free_queued_response_payloads(call);
    grpc_protocol_free_response_metadata_entries(call);
    if (call->method_path != NULL) {
        zend_string_release(call->method_path);
        call->method_path = NULL;
    }
    if (call->grpc_message != NULL) {
        zend_string_release(call->grpc_message);
        call->grpc_message = NULL;
    }
    if (call->content_type != NULL) {
        zend_string_release(call->content_type);
        call->content_type = NULL;
    }
    if (call->grpc_encoding != NULL) {
        zend_string_release(call->grpc_encoding);
        call->grpc_encoding = NULL;
    }
    smart_str_free(&call->body);
}
