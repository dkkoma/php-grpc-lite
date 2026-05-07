#include "internal.h"

/*
 * HTTP/2 transport helpers included by main.c.
 *
 * This file intentionally shares main.c's static scope: it owns connection
 * lifecycle, socket/TLS I/O, nghttp2 callbacks, request header assembly,
 * response frame parsing, and metadata storage helpers. PHP entrypoints and
 * module registration stay in main.c.
 */

#define GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES (64 * 1024 * 1024)
#define GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE 65535
#define GRPC_LITE_HTTP2_MAX_WINDOW_SIZE 2147483647L
#define GRPC_LITE_MAX_REQUEST_METADATA_VALUES 256
#define GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES 128
#define GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES (64 * 1024)
#define GRPC_LITE_DEFAULT_METADATA_SOFT_BYTES (8 * 1024)
#define GRPC_LITE_DEFAULT_METADATA_HARD_BYTES (16 * 1024)
#define GRPC_LITE_MAX_PERSISTENT_CONNECTIONS 128

static void destroy_h2_connection(h2_connection *connection);
static void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection);
static void detach_persistent_connection_by_ptr(h2_connection *connection);
static bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *stream);
static bool connection_owned_by_call(h2_connection *connection, grpc_call *call);
static void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *stream);
static void clear_connection_call_owner(h2_connection *connection, grpc_call *call);
static void cancel_active_server_streaming_call_state(server_streaming_call_state *stream, uint32_t error_code);
static void destroy_server_streaming_call_state(server_streaming_call_state *stream);
static void server_streaming_call_state_dtor(zend_resource *rsrc);
static int configure_callbacks(nghttp2_session_callbacks **callbacks);
static void mark_connection_dead(h2_connection *connection, int error_code);
static void set_connection_error_detail(h2_connection *connection, const char *detail);
static void mark_connection_draining(h2_connection *connection, int32_t last_stream_id, uint32_t error_code);
static bool connection_usable(h2_connection *connection);
static zend_ulong hash_bytes(const char *data, size_t data_len);
static void build_authority(char *buffer, size_t buffer_len, const char *host, zend_long port, const char *authority, size_t authority_len);
static persistent_connection_entry *create_persistent_connection_entry(h2_connection *connection, const char *host, zend_long port, bool use_tls, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len);
static bool connection_entry_matches_identity(persistent_connection_entry *entry, const char *host, zend_long port, bool use_tls, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len);
static bool preflight_persistent_connection(h2_connection *connection);
static void remove_unusable_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
static int set_socket_timeout_us(int fd, zend_long timeout_us);
static int set_fd_nonblocking_mode(int fd, bool nonblocking);
static int poll_timeout_ms_for_deadline(uint64_t deadline_abs_us);
static zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us);
static size_t effective_max_receive_message_bytes(zend_long max_receive_message_length);
static uint32_t effective_http2_window_size(zend_long configured);
static size_t effective_max_response_metadata_bytes(zend_long soft_limit, zend_long hard_limit);
static int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us);
static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len);
static int configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len);
static int configure_tls_connection(h2_connection *connection, const char *host, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us);
static ssize_t connection_send(grpc_call *call, const uint8_t *data, size_t length);
static ssize_t h2_connection_send(h2_connection *connection, const uint8_t *data, size_t length, uint64_t deadline_abs_us, bool *timed_out);
static ssize_t connection_recv(h2_connection *connection, uint8_t *data, size_t length, uint64_t deadline_abs_us);
static h2_connection *create_h2_connection(const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message);
static h2_connection *get_persistent_connection(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message);
static void discard_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
static int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us);
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static size_t remaining_request_bytes(grpc_call *call);
static size_t copy_request_bytes(grpc_call *call, uint8_t *buf, size_t length);
static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
static void grpc_protocol_set_message_header(grpc_call *call, size_t payload_len);
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);
static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data);
static uint64_t monotonic_us(void);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
#endif
static int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen);
static const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len);
static const char *validate_grpc_path(const char *path, size_t path_len);
static size_t count_custom_header_values(zval *headers_zv);
static int init_request_headers(h2_request_headers *headers, size_t custom_values);
static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen);
static void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us);
static int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);
static void free_request_headers(h2_request_headers *headers);
static int grpc_protocol_validate_response_message_lengths(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
static int grpc_protocol_process_response_data_direct(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
static int enqueue_response_payload(grpc_call *call, zend_string *payload);
static int deliver_response_payload(grpc_call *call, zend_string *payload, uint64_t ready_abs_us);
static int deliver_queued_response_payloads(grpc_call *call);
static void free_queued_response_payloads(grpc_call *call);
static void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call);
static int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
static void grpc_protocol_free_response_metadata_entries(grpc_call *call);
static void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing);
static void resolve_grpc_call_status(grpc_call *call, bool cancelled, grpc_lite_status_result *result);
static void add_status_result_to_return(zval *return_value, grpc_lite_status_result *status);
static void cleanup_grpc_call(grpc_call *call);

static grpc_call *active_grpc_call_from_session_user_data(void *user_data)
{
    h2_connection *connection = (h2_connection *) user_data;
    return connection != NULL ? connection->active_call : NULL;
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
    if (connection->session != NULL) {
        nghttp2_session_del(connection->session);
    }
    if (connection->callbacks != NULL) {
        nghttp2_session_callbacks_del(connection->callbacks);
    }
    pefree(connection, connection->persistent);
}

static void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection)
{
    if (entry == NULL) {
        return;
    }
    if (destroy_connection && entry->connection != NULL) {
        destroy_h2_connection(entry->connection);
    }
    if (entry->host_identity != NULL) {
        zend_string_release_ex(entry->host_identity, true);
    }
    if (entry->authority_identity != NULL) {
        zend_string_release_ex(entry->authority_identity, true);
    }
    if (entry->tls_verify_name_identity != NULL) {
        zend_string_release_ex(entry->tls_verify_name_identity, true);
    }
    if (entry->root_certs_identity != NULL) {
        zend_string_release_ex(entry->root_certs_identity, true);
    }
    if (entry->cert_chain_identity != NULL) {
        zend_string_release_ex(entry->cert_chain_identity, true);
    }
    if (entry->private_key_identity != NULL) {
        zend_string_release_ex(entry->private_key_identity, true);
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

static bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *stream)
{
    return connection != NULL && stream != NULL && connection->active_call == &stream->call;
}

static bool connection_owned_by_call(h2_connection *connection, grpc_call *call)
{
    return connection != NULL && call != NULL && connection->active_call == call;
}

static void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *stream)
{
    h2_connection *connection;

    if (stream == NULL || stream->connection == NULL) {
        return;
    }
    connection = stream->connection;
    if (!connection_owned_by_server_streaming_call_state(connection, stream)) {
        return;
    }

    if (!connection_usable(connection)) {
        detach_persistent_connection_by_ptr(connection);
    }
    connection->busy = false;
    connection->active_call = NULL;
    stream->connection = NULL;
    stream->call.connection = NULL;
    if (connection->detached_from_cache) {
        destroy_h2_connection(connection);
    }
}

static void clear_connection_call_owner(h2_connection *connection, grpc_call *call)
{
    if (!connection_owned_by_call(connection, call)) {
        return;
    }
    connection->busy = false;
    connection->active_call = NULL;
}

static void cancel_active_server_streaming_call_state(server_streaming_call_state *stream, uint32_t error_code)
{
    int rv;

    if (stream == NULL || stream->completed || !connection_owned_by_server_streaming_call_state(stream->connection, stream) || !connection_usable(stream->connection) || stream->call.stream_id <= 0) {
        return;
    }
    rv = nghttp2_submit_rst_stream(stream->connection->session, NGHTTP2_FLAG_NONE, stream->call.stream_id, error_code);
    if (rv != 0) {
        mark_connection_dead(stream->connection, rv);
        return;
    }
    rv = nghttp2_session_send(stream->connection->session);
    if (rv != 0) {
        mark_connection_dead(stream->connection, rv);
    }
}

static void destroy_server_streaming_call_state(server_streaming_call_state *stream)
{
    if (stream == NULL) {
        return;
    }
    if (!stream->completed && !stream->call.stream_closed && connection_owned_by_server_streaming_call_state(stream->connection, stream) && connection_usable(stream->connection) && stream->call.stream_id > 0) {
        set_connection_error_detail(stream->connection, "active stream resource destroyed before completion");
        mark_connection_dead(stream->connection, NGHTTP2_CANCEL);
        detach_persistent_connection_by_ptr(stream->connection);
    }
    clear_connection_server_streaming_call_state_owner(stream);
    if (stream->request != NULL) {
        zend_string_release(stream->request);
    }
    if (stream->recv_buf != NULL) {
        efree(stream->recv_buf);
    }
    cleanup_grpc_call(&stream->call);
    efree(stream);
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
    nghttp2_session_callbacks_set_on_frame_send_callback(*callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(*callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(*callbacks, on_frame_not_send_callback);
    return 0;
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

static zend_ulong hash_bytes(const char *data, size_t data_len)
{
    zend_ulong hash = (zend_ulong) 1469598103934665603ULL;
    size_t i;

    if (data == NULL || data_len == 0) {
        return 0;
    }
    for (i = 0; i < data_len; i++) {
        hash ^= (unsigned char) data[i];
        hash *= (zend_ulong) 1099511628211ULL;
    }
    return hash;
}

static void build_authority(char *buffer, size_t buffer_len, const char *host, zend_long port, const char *authority, size_t authority_len)
{
    if (authority != NULL && authority_len > 0) {
        size_t copy_len = authority_len < buffer_len - 1 ? authority_len : buffer_len - 1;
        memcpy(buffer, authority, copy_len);
        buffer[copy_len] = '\0';
        return;
    }

    snprintf(buffer, buffer_len, "%s:%ld", host, port);
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

static persistent_connection_entry *create_persistent_connection_entry(h2_connection *connection, const char *host, zend_long port, bool use_tls, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len)
{
    persistent_connection_entry *entry;
    char built_authority[512];

    if (connection == NULL) {
        return NULL;
    }

    entry = pecalloc(1, sizeof(persistent_connection_entry), true);
    if (entry == NULL) {
        return NULL;
    }

    build_authority(built_authority, sizeof(built_authority), host, port, authority, authority_len);
    entry->connection = connection;
    entry->host_len = strlen(host);
    entry->host_hash = hash_bytes(host, entry->host_len);
    entry->host_identity = zend_string_init(host, entry->host_len, true);
    entry->port = port;
    entry->authority_len = strlen(built_authority);
    entry->authority_hash = hash_bytes(built_authority, entry->authority_len);
    entry->authority_identity = zend_string_init(built_authority, entry->authority_len, true);
    entry->use_tls = use_tls;
    entry->tls_verify_name_len = tls_verify_name_len;
    entry->tls_verify_name_hash = hash_bytes(tls_verify_name, tls_verify_name_len);
    entry->tls_verify_name_identity = zend_string_init(tls_verify_name != NULL ? tls_verify_name : "", tls_verify_name_len, true);
    entry->root_certs_len = root_certs_len;
    entry->root_certs_hash = hash_bytes(root_certs, root_certs_len);
    entry->root_certs_identity = zend_string_init(root_certs != NULL ? root_certs : "", root_certs_len, true);
    entry->cert_chain_len = cert_chain_len;
    entry->cert_chain_hash = hash_bytes(cert_chain, cert_chain_len);
    entry->cert_chain_identity = zend_string_init(cert_chain != NULL ? cert_chain : "", cert_chain_len, true);
    entry->private_key_len = private_key_len;
    entry->private_key_hash = hash_bytes(private_key, private_key_len);
    entry->private_key_identity = zend_string_init(private_key != NULL ? private_key : "", private_key_len, true);

    if (entry->host_identity == NULL
        || entry->authority_identity == NULL
        || entry->tls_verify_name_identity == NULL
        || entry->root_certs_identity == NULL
        || entry->cert_chain_identity == NULL
        || entry->private_key_identity == NULL) {
        destroy_persistent_connection_entry(entry, false);
        return NULL;
    }

    return entry;
}

static bool connection_entry_matches_identity(persistent_connection_entry *entry, const char *host, zend_long port, bool use_tls, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len)
{
    char expected_authority[512];
    size_t host_len;

    if (entry == NULL || entry->connection == NULL) {
        return false;
    }
    build_authority(expected_authority, sizeof(expected_authority), host, port, authority, authority_len);
    host_len = strlen(host);

    return entry->use_tls == use_tls
        && entry->port == port
        && entry->host_len == host_len
        && entry->host_hash == hash_bytes(host, host_len)
        && identity_matches(entry->host_identity, host, host_len)
        && entry->authority_len == strlen(expected_authority)
        && entry->authority_hash == hash_bytes(expected_authority, strlen(expected_authority))
        && identity_matches(entry->authority_identity, expected_authority, strlen(expected_authority))
        && entry->tls_verify_name_len == tls_verify_name_len
        && entry->tls_verify_name_hash == hash_bytes(tls_verify_name, tls_verify_name_len)
        && identity_matches(entry->tls_verify_name_identity, tls_verify_name, tls_verify_name_len)
        && entry->root_certs_len == root_certs_len
        && entry->root_certs_hash == hash_bytes(root_certs, root_certs_len)
        && identity_matches(entry->root_certs_identity, root_certs, root_certs_len)
        && entry->cert_chain_len == cert_chain_len
        && entry->cert_chain_hash == hash_bytes(cert_chain, cert_chain_len)
        && identity_matches(entry->cert_chain_identity, cert_chain, cert_chain_len)
        && entry->private_key_len == private_key_len
        && entry->private_key_hash == hash_bytes(private_key, private_key_len)
        && identity_matches(entry->private_key_identity, private_key, private_key_len);
}

static bool preflight_persistent_connection(h2_connection *connection)
{
    char byte;
    ssize_t rv;

    if (!connection_usable(connection) || connection->busy) {
        return connection_usable(connection);
    }
    if (connection->ssl != NULL) {
        int ssl_error;
        int previous_mode = fcntl(connection->fd, F_GETFL, 0);
        if (set_fd_nonblocking_mode(connection->fd, true) != 0) {
            mark_connection_dead(connection, errno);
            return false;
        }
        rv = SSL_peek(connection->ssl, &byte, sizeof(byte));
        ssl_error = SSL_get_error(connection->ssl, (int) rv);
        if (previous_mode >= 0) {
            fcntl(connection->fd, F_SETFL, previous_mode);
        } else {
            set_fd_nonblocking_mode(connection->fd, false);
        }
        if (rv > 0) {
            mark_connection_draining(connection, 0, NGHTTP2_NO_ERROR);
            set_connection_error_detail(connection, "persistent TLS connection has pending control data before reuse");
            return false;
        }
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
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
        return false;
    }

    rv = recv(connection->fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (rv == 0) {
        set_connection_error_detail(connection, "persistent connection closed by peer before reuse");
        mark_connection_dead(connection, 0);
        return false;
    }
    if (rv < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        mark_connection_dead(connection, errno);
        return false;
    }
    if (rv > 0) {
        mark_connection_draining(connection, 0, NGHTTP2_NO_ERROR);
        set_connection_error_detail(connection, "persistent connection has pending control data before reuse");
        return false;
    }

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
    if (connection->busy) {
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

static int set_socket_timeout_us(int fd, zend_long timeout_us)
{
    struct timeval tv;
    if (timeout_us <= 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec = (time_t) (timeout_us / 1000000);
        tv.tv_usec = (suseconds_t) (timeout_us % 1000000);
        if (tv.tv_sec == 0 && tv.tv_usec == 0) {
            tv.tv_usec = 1;
        }
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
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

static size_t effective_max_receive_message_bytes(zend_long max_receive_message_length)
{
    if (max_receive_message_length == -1) {
        return SIZE_MAX;
    }
    if (max_receive_message_length > 0) {
        return (size_t) max_receive_message_length;
    }
    return GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES;
}

static uint32_t effective_http2_window_size(zend_long configured)
{
    if (configured < GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE;
    }
    if (configured > GRPC_LITE_HTTP2_MAX_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_MAX_WINDOW_SIZE;
    }
    return (uint32_t) configured;
}

static size_t effective_max_response_metadata_bytes(zend_long soft_limit, zend_long hard_limit)
{
    if (hard_limit >= 0) {
        return (size_t) hard_limit;
    }
    if (soft_limit >= 0) {
        double derived = (double) soft_limit * 1.25;
        if (derived > (double) SIZE_MAX) {
            return SIZE_MAX;
        }
        if (derived < GRPC_LITE_DEFAULT_METADATA_HARD_BYTES) {
            return GRPC_LITE_DEFAULT_METADATA_HARD_BYTES;
        }
        return (size_t) derived;
    }
    return GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES;
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
    set_fd_nonblocking_mode(connection->fd, false);
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
    if (set_socket_timeout_us(connection->fd, remaining_timeout_us) != 0) {
        return -1;
    }
    if (connection->ssl != NULL) {
        if (length > INT_MAX) {
            errno = EMSGSIZE;
            connection->last_io_errno = errno;
            set_connection_error_detail(connection, "SSL_write length exceeds INT_MAX");
            return -1;
        }
        int written = SSL_write(connection->ssl, data, (int) length);
        if (written <= 0) {
            int ssl_error = SSL_get_error(connection->ssl, written);
            connection->last_ssl_error = ssl_error;
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && deadline_abs_us > 0) {
                if (timed_out != NULL) {
                    *timed_out = true;
                }
                errno = ETIMEDOUT;
            }
            connection->last_io_errno = errno;
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_write failed: SSL_get_error=%d", ssl_error);
            return -1;
        }
        return written;
    }
    ssize_t written = send(connection->fd, data, length,
#ifdef MSG_NOSIGNAL
        MSG_NOSIGNAL
#else
        0
#endif
    );
    if (written < 0) {
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && deadline_abs_us > 0) {
            if (timed_out != NULL) {
                *timed_out = true;
            }
            errno = ETIMEDOUT;
        }
        connection->last_io_errno = errno;
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "send failed: %s", strerror(errno));
    }
    return written;
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
    if (set_socket_timeout_us(connection->fd, remaining_timeout_us) != 0) {
        return -1;
    }
    if (connection->ssl != NULL) {
        int nread = SSL_read(connection->ssl, data, (int) length);
        if (nread <= 0) {
            int ssl_error = SSL_get_error(connection->ssl, nread);
            connection->last_ssl_error = ssl_error;
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && deadline_abs_us > 0) {
                errno = ETIMEDOUT;
            }
            connection->last_io_errno = errno;
            snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "SSL_read failed: SSL_get_error=%d", ssl_error);
            return -1;
        }
        return nread;
    }
    ssize_t nread = recv(connection->fd, data, length, 0);
    if (nread < 0) {
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && deadline_abs_us > 0) {
            errno = ETIMEDOUT;
        }
        connection->last_io_errno = errno;
        snprintf(connection->last_error_detail, sizeof(connection->last_error_detail), "recv failed: %s", strerror(errno));
    }
    return nread;
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
    rv = nghttp2_session_send(connection->session);
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

    *persistent_reused = false;
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
    if (connection != NULL && !preflight_persistent_connection(connection)) {
        remove_unusable_persistent_connection(key, key_len, connection);
        entry = NULL;
        connection = NULL;
    }
    if (connection != NULL && !connection_entry_matches_identity(entry, host, port, use_tls, authority, authority_len, tls_verify_name, tls_verify_name_len, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len)) {
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
        entry = create_persistent_connection_entry(connection, host, port, use_tls, authority, authority_len, tls_verify_name, tls_verify_name_len, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len);
        if (entry == NULL) {
            destroy_h2_connection(connection);
            *error_message = "failed to allocate persistent connection entry";
            return NULL;
        }
        zend_hash_str_update_ptr(&PHP_GRPC_LITE_G(persistent_connections), key, key_len, entry);
        return connection;
    }

    *persistent_reused = true;
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
    if (connection->busy) {
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
    grpc_call *call;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    if (connection == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    call = connection->active_call;

    while (total_written < length) {
        ssize_t written = call != NULL
            ? connection_send(call, data + total_written, length - total_written)
            : h2_connection_send(connection, data + total_written, length - total_written, connection->setup_deadline_abs_us, &connection->setup_timed_out);
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        total_written += (size_t) written;
    }
    if (call != NULL) {
        call->bytes_sent += total_written;
    }
    return (ssize_t) total_written;
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
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    size_t total_len;
    size_t remaining;
    size_t to_send;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    if (call == NULL) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    total_len = call->grpc_header_len + call->request_len;
    remaining = remaining_request_bytes(call);
    to_send = remaining < length ? remaining : length;
    call->data_read_calls++;

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
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    (void) session;
    (void) flags;
    if (call == NULL) {
        return 0;
    }
    if (stream_id == call->stream_id && len > 0) {
        call->data_recv_calls++;
        if (call->direct_response_payload && call->decode_response_incrementally && ((call->payload_callback_fci != NULL && call->payload_callback_fcc != NULL) || call->queue_response_payloads)) {
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
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
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
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    (void) session;
    (void) error_code;
    if (call == NULL) {
        return 0;
    }
    if (stream_id == call->stream_id) {
        call->stream_closed = true;
        call->stream_error_code = error_code;
    }
    return 0;
}

static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    (void) session;
    if (call == NULL) {
        return 0;
    }
    call->sent_frames++;
    call->last_sent_frame_type = frame->hd.type;
    call->last_sent_frame_flags = frame->hd.flags;
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    (void) session;
    if (call == NULL) {
        return 0;
    }
    call->recv_frames++;
    call->last_recv_frame_type = frame->hd.type;
    call->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        mark_connection_draining(call->connection, frame->goaway.last_stream_id, frame->goaway.error_code);
        if (call->stream_id > 0 && frame->goaway.last_stream_id < call->stream_id) {
            call->stream_error_code = NGHTTP2_REFUSED_STREAM;
            call->stream_closed = true;
        }
    } else if (frame->hd.type == NGHTTP2_HEADERS
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

static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    grpc_call *call = active_grpc_call_from_session_user_data(user_data);
    (void) session;
    if (call == NULL) {
        return 0;
    }
    call->not_sent_frames++;
    call->last_not_sent_frame_type = frame->hd.type;
    call->last_not_sent_error = lib_error_code;
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

static int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen)
{
    int status = 0;

    if (valuelen == 0 || valuelen > 2 || (valuelen > 1 && value[0] == '0')) {
        return -1;
    }

    for (size_t i = 0; i < valuelen; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return -1;
        }
        status = (status * 10) + (value[i] - '0');
    }

    return status <= 16 ? status : -1;
}

static bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen)
{
    static const char prefix[] = "application/grpc";
    size_t prefix_len = sizeof(prefix) - 1;

    if (valuelen < prefix_len || strncasecmp((const char *) value, prefix, prefix_len) != 0) {
        return false;
    }
    if (valuelen == prefix_len) {
        return true;
    }
    return (value[prefix_len] == '+' && valuelen > prefix_len + 1) || value[prefix_len] == ';';
}

static bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen)
{
    return valuelen == sizeof("identity") - 1 && strncasecmp((const char *) value, "identity", sizeof("identity") - 1) == 0;
}

static int grpc_lite_hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

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

static int grpc_lite_status_code_from_call(grpc_call *call, bool cancelled)
{
    if (call->timed_out) return GRPC_STATUS_DEADLINE_EXCEEDED;
    if (cancelled) return GRPC_STATUS_CANCELLED;
    if (call->invalid_grpc_status) return GRPC_STATUS_UNKNOWN;
    if (call->grpc_status < 0 && call->http_status != 200) {
        switch (call->http_status) {
            case 400: return GRPC_STATUS_INTERNAL;
            case 401: return GRPC_STATUS_UNAUTHENTICATED;
            case 403: return GRPC_STATUS_PERMISSION_DENIED;
            case 404: return GRPC_STATUS_UNIMPLEMENTED;
            case 429:
            case 502:
            case 503:
            case 504:
                return GRPC_STATUS_UNAVAILABLE;
            default:
                return call->http_status < 0 ? GRPC_STATUS_UNAVAILABLE : GRPC_STATUS_UNKNOWN;
        }
    }
    if (call->invalid_content_type) return GRPC_STATUS_UNKNOWN;
    if (call->response_message_too_large || call->metadata_too_large) return GRPC_STATUS_RESOURCE_EXHAUSTED;
    if (call->malformed_response_frame) return GRPC_STATUS_INTERNAL;
    if (call->compressed_response_seen || call->unsupported_response_encoding) return GRPC_STATUS_UNIMPLEMENTED;
    if (call->grpc_status >= 0) return call->grpc_status;
    if (call->stream_reset_seen) {
        switch (call->stream_error_code) {
            case NGHTTP2_CANCEL:
                return GRPC_STATUS_CANCELLED;
            case NGHTTP2_REFUSED_STREAM:
                return GRPC_STATUS_UNAVAILABLE;
            case NGHTTP2_ENHANCE_YOUR_CALM:
                return GRPC_STATUS_RESOURCE_EXHAUSTED;
            case NGHTTP2_INADEQUATE_SECURITY:
                return GRPC_STATUS_PERMISSION_DENIED;
            case NGHTTP2_NO_ERROR:
            case NGHTTP2_PROTOCOL_ERROR:
            case NGHTTP2_INTERNAL_ERROR:
            case NGHTTP2_FLOW_CONTROL_ERROR:
            case NGHTTP2_SETTINGS_TIMEOUT:
            case NGHTTP2_STREAM_CLOSED:
            case NGHTTP2_FRAME_SIZE_ERROR:
            case NGHTTP2_COMPRESSION_ERROR:
            case NGHTTP2_CONNECT_ERROR:
                return GRPC_STATUS_INTERNAL;
            default:
                return GRPC_STATUS_UNKNOWN;
        }
    }
    return GRPC_STATUS_UNKNOWN;
}

static zend_string *grpc_lite_status_details_from_call(grpc_call *call, int code)
{
    if (call->grpc_message != NULL && ZSTR_LEN(call->grpc_message) > 0) {
        return zend_string_copy(call->grpc_message);
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
        case GRPC_STATUS_RESOURCE_EXHAUSTED:
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
            if (call->stream_reset_seen) {
                return strpprintf(0, "HTTP/2 stream reset: %u", call->stream_error_code);
            }
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

static bool contains_nul_or_control(const char *value, size_t value_len)
{
    size_t index;

    if (value == NULL) {
        return false;
    }
    for (index = 0; index < value_len; index++) {
        unsigned char ch = (unsigned char) value[index];
        if (ch == '\0' || ch < 0x20 || ch == 0x7f) {
            return true;
        }
    }
    return false;
}

static bool contains_authority_forbidden_char(const char *value, size_t value_len)
{
    size_t index;

    if (value == NULL) {
        return false;
    }
    for (index = 0; index < value_len; index++) {
        unsigned char ch = (unsigned char) value[index];
        if (ch == '@' || ch == '/' || ch == '\\' || ch <= 0x20 || ch == 0x7f) {
            return true;
        }
    }
    return false;
}

static const char *validate_grpc_path(const char *path, size_t path_len)
{
    bool second_slash_seen = false;
    size_t index;

    if (path == NULL || path_len < 3 || path[0] != '/') {
        return "invalid gRPC method path";
    }
    for (index = 0; index < path_len; index++) {
        unsigned char ch = (unsigned char) path[index];
        if (ch <= 0x20 || ch == 0x7f) {
            return "invalid gRPC method path";
        }
        if (index > 0 && ch == '/') {
            second_slash_seen = true;
        }
    }
    return second_slash_seen ? NULL : "invalid gRPC method path";
}

static const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len)
{
    char port_buf[32];
    int port_len;

    if (key_len == 0 || key_len > 512 || contains_nul_or_control(key, key_len)) {
        return "invalid grpc_lite connection key";
    }
    if (host_len == 0 || contains_nul_or_control(host, host_len)) {
        return "invalid gRPC target host";
    }
    if (port <= 0 || port > 65535) {
        return "invalid gRPC target port";
    }
    if (authority_len > 0) {
        if (authority_len >= sizeof(((h2_connection *) 0)->authority) || contains_authority_forbidden_char(authority, authority_len)) {
            return "invalid gRPC authority";
        }
    } else {
        port_len = snprintf(port_buf, sizeof(port_buf), "%ld", port);
        if (port_len < 0 || host_len + 1 + (size_t) port_len >= sizeof(((h2_connection *) 0)->authority)) {
            return "gRPC authority is too long";
        }
    }
    if (tls_verify_name_len > 0 && contains_authority_forbidden_char(tls_verify_name, tls_verify_name_len)) {
        return "invalid TLS verify name";
    }

    return NULL;
}

static size_t count_custom_header_values(zval *headers_zv)
{
    size_t count = 0;
    zend_string *key;
    zval *value;

    if (headers_zv == NULL || Z_TYPE_P(headers_zv) != IS_ARRAY) {
        return 0;
    }

    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
        if (key == NULL) {
            continue;
        }
        if (Z_TYPE_P(value) == IS_ARRAY) {
            zval *nested;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), nested) {
                count++;
            } ZEND_HASH_FOREACH_END();
            continue;
        }
        count++;
    } ZEND_HASH_FOREACH_END();

    return count;
}

static int init_request_headers(h2_request_headers *headers, size_t custom_values)
{
    if (custom_values > GRPC_LITE_MAX_REQUEST_METADATA_VALUES || custom_values > SIZE_MAX - 7) {
        zend_throw_exception(NULL, "gRPC request metadata exceeds maximum count", 0);
        return -1;
    }
    headers->capacity = 7 + custom_values;
    headers->len = 0;
    headers->name_count = 0;
    headers->value_count = 0;
    headers->nva = ecalloc(headers->capacity == 0 ? 1 : headers->capacity, sizeof(nghttp2_nv));
    headers->name_strings = custom_values > 0 ? ecalloc(custom_values, sizeof(zend_string *)) : NULL;
    headers->value_strings = custom_values > 0 ? ecalloc(custom_values, sizeof(zend_string *)) : NULL;
    return 0;
}

static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen)
{
    if (headers->len >= headers->capacity) {
        return;
    }
    headers->nva[headers->len++] = (nghttp2_nv) {
        (uint8_t *) name,
        (uint8_t *) value,
        namelen,
        valuelen,
        NGHTTP2_NV_FLAG_NONE
    };
}

static zend_long grpc_lite_ceil_div_timeout(zend_long value, zend_long unit)
{
    return value / unit + (value % unit != 0 ? 1 : 0);
}

static void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us)
{
    char timeout_buf[32];
    zend_long value;
    char unit;
    if (timeout_us <= 0) {
        return;
    }
    if (timeout_us <= 99999999L) {
        value = timeout_us;
        unit = 'u';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000L);
        unit = 'm';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000000L);
        unit = 'S';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 60000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 60000000L);
        unit = 'M';
    } else {
        value = grpc_lite_ceil_div_timeout(timeout_us, 3600000000L);
        if (value > 99999999L) {
            value = 99999999L;
        }
        unit = 'H';
    }
    snprintf(timeout_buf, sizeof(timeout_buf), "%ld%c", value, unit);
    append_request_header(headers, "grpc-timeout", sizeof("grpc-timeout") - 1, timeout_buf, strlen(timeout_buf));
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
    size_t custom_capacity;

    if (Z_TYPE_P(value) != IS_STRING) {
        zend_throw_exception(NULL, "gRPC request metadata value must be a string", 0);
        return -1;
    }
    custom_capacity = headers->capacity >= 7 ? headers->capacity - 7 : 0;
    if (headers->name_strings == NULL || headers->value_strings == NULL || headers->name_count >= custom_capacity || headers->value_count >= custom_capacity) {
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
        efree(headers->name_strings);
    }
    if (headers->value_strings != NULL) {
        for (i = 0; i < headers->value_count; i++) {
            if (headers->value_strings[i] != NULL) {
                zend_string_release(headers->value_strings[i]);
            }
        }
        efree(headers->value_strings);
    }
    if (headers->nva != NULL) {
        efree(headers->nva);
    }
    headers->nva = NULL;
    headers->name_strings = NULL;
    headers->value_strings = NULL;
    headers->len = 0;
    headers->capacity = 0;
    headers->name_count = 0;
    headers->value_count = 0;
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
            uint64_t payload_copy_started = call->observe_payload_copy != NULL ? monotonic_us() : 0;
            if (take > 0) {
                memcpy(ZSTR_VAL(call->response_payload) + call->response_payload_offset, data + offset, take);
                call->response_payload_offset += take;
                offset += take;
            }
            if (call->observe_payload_copy != NULL) {
                call->observe_payload_copy(call, monotonic_us() - payload_copy_started);
            }

            if (call->response_payload_offset == call->response_payload_len) {
                zend_string *payload = call->response_payload;
                uint64_t ready_abs_us = monotonic_us();

                if (call->observe_message_ready != NULL) {
                    call->observe_message_ready(call, ready_abs_us);
                }
                ZSTR_VAL(payload)[call->response_payload_len] = '\0';
                call->response_payload = NULL;
                call->response_header_len = 0;
                call->response_payload_len = 0;
                call->response_payload_offset = 0;

                if (call->queue_response_payloads) {
                    if (enqueue_response_payload(call, payload) != 0) {
                        return -1;
                    }
                } else if (deliver_response_payload(call, payload, ready_abs_us) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int enqueue_response_payload(grpc_call *call, zend_string *payload)
{
    queued_payload *entry = emalloc(sizeof(queued_payload));
    entry->payload = payload;
    entry->ready_abs_us = monotonic_us();
    entry->next = NULL;

    if (call->response_queue_tail == NULL) {
        call->response_queue_head = entry;
    } else {
        call->response_queue_tail->next = entry;
    }
    call->response_queue_tail = entry;
    call->response_queue_count++;
    call->response_queue_bytes += ZSTR_LEN(payload);
    if (call->observe_payload_queued != NULL) {
        call->observe_payload_queued(call);
    }

    if (call->flush_queue_if_limited != NULL && call->flush_queue_if_limited(call) != 0) {
        return -1;
    }

    return 0;
}

static int deliver_response_payload(grpc_call *call, zend_string *payload, uint64_t ready_abs_us)
{
    zval params[1];
    zval retval;
    uint64_t started = call->observe_payload_delivered != NULL ? monotonic_us() : 0;

    ZVAL_STR(&params[0], payload);
    ZVAL_UNDEF(&retval);
    // cppcheck-suppress autoVariables
    call->payload_callback_fci->params = params;
    call->payload_callback_fci->param_count = 1;
    // cppcheck-suppress autoVariables
    call->payload_callback_fci->retval = &retval;

    if (zend_call_function(call->payload_callback_fci, call->payload_callback_fcc) != SUCCESS || EG(exception)) {
        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        return -1;
    }
    if (call->observe_payload_delivered != NULL) {
        uint64_t elapsed = monotonic_us() - started;
        call->observe_payload_delivered(call, ready_abs_us, started, elapsed);
    }

    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
    }

    return 0;
}

static int deliver_queued_response_payloads(grpc_call *call)
{
    while (call->response_queue_head != NULL) {
        queued_payload *entry = call->response_queue_head;
        call->response_queue_head = entry->next;
        if (call->response_queue_head == NULL) {
            call->response_queue_tail = NULL;
        }
        call->response_queue_count--;
        call->response_queue_bytes -= ZSTR_LEN(entry->payload);
        if (deliver_response_payload(call, entry->payload, entry->ready_abs_us) != 0) {
            efree(entry);
            return -1;
        }
        efree(entry);
    }

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

static void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing)
{
    zval metadata;
    metadata_entry *entry;

    array_init(&metadata);
    for (entry = call->metadata_head; entry != NULL; entry = entry->next) {
        if (entry->trailing != trailing) {
            continue;
        }
        if (is_binary_metadata_header(entry->key)) {
            add_binary_metadata_values_to_map(&metadata, entry->key, entry->value);
        } else {
            add_metadata_value_to_map(&metadata, entry->key, zend_string_copy(entry->value));
        }
    }

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
