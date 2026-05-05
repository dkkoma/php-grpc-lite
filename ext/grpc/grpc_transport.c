#include "grpc_internal.h"

/*
 * HTTP/2 transport helpers included by grpc.c.
 *
 * This file intentionally shares grpc.c's static scope: it owns channel
 * lifecycle, socket/TLS I/O, nghttp2 callbacks, request header assembly,
 * response frame parsing, and metadata storage helpers. PHP entrypoints and
 * module registration stay in grpc.c.
 */

static void destroy_h2_channel(h2_channel *channel);
static bool channel_owned_by_stream(h2_channel *channel, h2_stream *stream);
static bool channel_owned_by_call(h2_channel *channel, grpc_call *client);
static void clear_channel_stream_owner(h2_stream *stream);
static void clear_channel_call_owner(h2_channel *channel, grpc_call *client);
static void cancel_active_stream(h2_stream *stream, uint32_t error_code);
static void destroy_h2_stream(h2_stream *stream);
static void h2_stream_dtor(zend_resource *rsrc);
static int configure_callbacks(nghttp2_session_callbacks **callbacks);
static void mark_channel_dead(h2_channel *channel, int error_code);
static void set_channel_error_detail(h2_channel *channel, const char *detail);
static void mark_channel_draining(h2_channel *channel, int32_t last_stream_id, uint32_t error_code);
static bool channel_usable(h2_channel *channel);
static bool preflight_persistent_channel(h2_channel *channel);
static void remove_unusable_persistent_channel(const char *key, size_t key_len, h2_channel *channel);
static int set_socket_timeout_us(int fd, zend_long timeout_us);
static int set_fd_nonblocking_mode(int fd, bool nonblocking);
static int poll_timeout_ms_for_deadline(uint64_t deadline_abs_us);
static zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us);
static size_t effective_max_receive_message_bytes(zend_long max_receive_message_length);
static int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us);
static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len);
static int configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len);
static int configure_tls_channel(h2_channel *channel, const char *host, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us);
static ssize_t channel_send(grpc_call *client, const uint8_t *data, size_t length);
static ssize_t channel_recv(h2_channel *channel, uint8_t *data, size_t length, uint64_t deadline_abs_us);
static h2_channel *create_h2_channel(const char *host, zend_long port, const char *authority, size_t authority_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message);
static h2_channel *get_persistent_channel(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message);
static void discard_persistent_channel(const char *key, size_t key_len, h2_channel *channel);
static int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us);
static int set_nonblocking(int fd);
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static size_t remaining_request_bytes(grpc_call *client);
static size_t copy_request_bytes(grpc_call *client, uint8_t *buf, size_t length);
static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
static void set_grpc_header(grpc_call *client, size_t payload_len);
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
static int parse_grpc_status_value(const uint8_t *value, size_t valuelen);
static size_t count_custom_header_values(zval *headers_zv);
static void init_request_headers(h2_request_headers *headers, size_t custom_values);
static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen);
static int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);
static void free_request_headers(h2_request_headers *headers);
static int validate_response_message_lengths(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len);
static int process_response_data_direct(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len);
static int enqueue_response_payload(grpc_call *client, zend_string *payload);
static int deliver_response_payload(grpc_call *client, zend_string *payload, uint64_t ready_abs_us);
static int deliver_queued_response_payloads(grpc_call *client);
static void free_queued_response_payloads(grpc_call *client);
static int add_metadata_entry(grpc_call *client, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
static void free_metadata_entries(grpc_call *client);
static void add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *client, bool trailing);
static void cleanup_grpc_call(grpc_call *client);

static void destroy_h2_channel(h2_channel *channel)
{
    if (channel == NULL) {
        return;
    }
    if (channel->ssl != NULL) {
        SSL_shutdown(channel->ssl);
        SSL_free(channel->ssl);
    }
    if (channel->ssl_ctx != NULL) {
        SSL_CTX_free(channel->ssl_ctx);
    }
    if (channel->fd >= 0) {
        close(channel->fd);
    }
    if (channel->session != NULL) {
        nghttp2_session_del(channel->session);
    }
    if (channel->callbacks != NULL) {
        nghttp2_session_callbacks_del(channel->callbacks);
    }
    pefree(channel, channel->persistent);
}

static bool channel_owned_by_stream(h2_channel *channel, h2_stream *stream)
{
    return channel != NULL && stream != NULL && channel->active_stream_owner == stream;
}

static bool channel_owned_by_call(h2_channel *channel, grpc_call *client)
{
    return channel != NULL && client != NULL && channel->active_call_owner == client;
}

static void clear_channel_stream_owner(h2_stream *stream)
{
    h2_channel *channel;

    if (stream == NULL || stream->channel == NULL) {
        return;
    }
    channel = stream->channel;
    if (!channel_owned_by_stream(channel, stream)) {
        return;
    }

    if (channel->session != NULL) {
        nghttp2_session_set_user_data(channel->session, NULL);
    }
    channel->busy = false;
    channel->active_stream_owner = NULL;
    if (channel->detached_from_cache) {
        stream->channel = NULL;
        destroy_h2_channel(channel);
    }
}

static void clear_channel_call_owner(h2_channel *channel, grpc_call *client)
{
    if (!channel_owned_by_call(channel, client)) {
        return;
    }
    if (channel->session != NULL) {
        nghttp2_session_set_user_data(channel->session, NULL);
    }
    channel->busy = false;
    channel->active_call_owner = NULL;
}

static void cancel_active_stream(h2_stream *stream, uint32_t error_code)
{
    int rv;

    if (stream == NULL || stream->completed || !channel_owned_by_stream(stream->channel, stream) || !channel_usable(stream->channel) || stream->client.stream_id <= 0) {
        return;
    }
    rv = nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, error_code);
    if (rv != 0) {
        mark_channel_dead(stream->channel, rv);
        return;
    }
    rv = nghttp2_session_send(stream->channel->session);
    if (rv != 0) {
        mark_channel_dead(stream->channel, rv);
    }
}

static void destroy_h2_stream(h2_stream *stream)
{
    if (stream == NULL) {
        return;
    }
    cancel_active_stream(stream, NGHTTP2_CANCEL);
    clear_channel_stream_owner(stream);
    if (stream->request != NULL) {
        zend_string_release(stream->request);
    }
    if (stream->recv_buf != NULL) {
        efree(stream->recv_buf);
    }
    cleanup_grpc_call(&stream->client);
    efree(stream);
}

static void h2_stream_dtor(zend_resource *rsrc)
{
    destroy_h2_stream((h2_stream *) rsrc->ptr);
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

static void mark_channel_dead(h2_channel *channel, int error_code)
{
    if (channel == NULL) {
        return;
    }
    channel->dead = true;
    channel->last_error = error_code;
    channel->last_io_errno = errno;
    if (channel->last_error_detail[0] == '\0' && errno != 0) {
        snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "%s", strerror(errno));
    }
}

static void set_channel_error_detail(h2_channel *channel, const char *detail)
{
    if (channel == NULL || detail == NULL) {
        return;
    }
    snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "%s", detail);
}

static void mark_channel_draining(h2_channel *channel, int32_t last_stream_id, uint32_t error_code)
{
    if (channel == NULL) {
        return;
    }
    channel->draining = true;
    channel->last_goaway_stream_id = last_stream_id;
    channel->last_goaway_error_code = error_code;
}

static bool channel_usable(h2_channel *channel)
{
    return channel != NULL && channel->fd >= 0 && channel->session != NULL && !channel->dead && !channel->draining;
}

static bool preflight_persistent_channel(h2_channel *channel)
{
    char byte;
    ssize_t rv;

    if (!channel_usable(channel) || channel->busy) {
        return channel_usable(channel);
    }
    if (channel->ssl != NULL) {
        return true;
    }

    rv = recv(channel->fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (rv == 0) {
        set_channel_error_detail(channel, "persistent channel closed by peer before reuse");
        mark_channel_dead(channel, 0);
        return false;
    }
    if (rv < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        mark_channel_dead(channel, errno);
        return false;
    }
    if (rv > 0) {
        mark_channel_draining(channel, 0, NGHTTP2_NO_ERROR);
        set_channel_error_detail(channel, "persistent channel has pending control data before reuse");
        return false;
    }

    return true;
}

static void remove_unusable_persistent_channel(const char *key, size_t key_len, h2_channel *channel)
{
    if (channel == NULL || channel_usable(channel)) {
        return;
    }
    if (PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        zend_hash_str_del(&PHP_GRPC_LITE_G(persistent_channels), key, key_len);
    }
    if (channel->busy) {
        channel->detached_from_cache = true;
        return;
    }
    destroy_h2_channel(channel);
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
            return pfd.revents & (events | POLLERR | POLLHUP | POLLNVAL) ? 0 : -1;
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

static int configure_tls_channel(h2_channel *channel, const char *host, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us)
{
    channel->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (channel->ssl_ctx == NULL) {
        set_channel_error_detail(channel, "failed to create SSL_CTX");
        return -1;
    }
    SSL_CTX_set_verify(channel->ssl_ctx, SSL_VERIFY_PEER, NULL);

    if (root_certs != NULL && root_certs_len > 0) {
        if (add_pem_certs_to_store(SSL_CTX_get_cert_store(channel->ssl_ctx), root_certs, root_certs_len) != 0) {
            set_channel_error_detail(channel, "failed to load root certificates");
            return -1;
        }
    } else if (SSL_CTX_set_default_verify_paths(channel->ssl_ctx) != 1) {
        set_channel_error_detail(channel, "failed to load default root certificates");
        return -1;
    }

    if (cert_chain != NULL && private_key != NULL && cert_chain_len > 0 && private_key_len > 0) {
        if (configure_client_certificate(channel->ssl_ctx, cert_chain, cert_chain_len, private_key, private_key_len) != 0) {
            set_channel_error_detail(channel, "failed to configure client certificate");
            return -1;
        }
    }

    channel->ssl = SSL_new(channel->ssl_ctx);
    if (channel->ssl == NULL) {
        set_channel_error_detail(channel, "failed to create SSL object");
        return -1;
    }
    static const unsigned char alpn[] = {2, 'h', '2'};
    if (SSL_set_alpn_protos(channel->ssl, alpn, sizeof(alpn)) != 0) {
        set_channel_error_detail(channel, "failed to configure TLS ALPN h2");
        return -1;
    }
    if (SSL_set_tlsext_host_name(channel->ssl, host) != 1) {
        set_channel_error_detail(channel, "failed to configure TLS SNI host");
        return -1;
    }
    if (SSL_set1_host(channel->ssl, host) != 1) {
        set_channel_error_detail(channel, "failed to configure TLS verification host");
        return -1;
    }
    if (SSL_set_fd(channel->ssl, channel->fd) != 1) {
        set_channel_error_detail(channel, "failed to attach TLS socket");
        return -1;
    }
    if (set_fd_nonblocking_mode(channel->fd, true) != 0) {
        set_channel_error_detail(channel, "failed to set TLS socket nonblocking");
        return -1;
    }
    while (true) {
        int rv = SSL_connect(channel->ssl);
        if (rv == 1) {
            break;
        }
        int ssl_error = SSL_get_error(channel->ssl, rv);
        channel->last_ssl_error = ssl_error;
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
            if (poll_fd_until_deadline(channel->fd, events, deadline_abs_us) == 0) {
                continue;
            }
            channel->last_io_errno = errno;
            if (errno == ETIMEDOUT) {
                set_channel_error_detail(channel, "HTTP/2 transport deadline exceeded");
            } else {
                snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "TLS handshake poll failed: %s", strerror(errno));
            }
            set_fd_nonblocking_mode(channel->fd, false);
            return -1;
        }
        channel->tls_verify_result = SSL_get_verify_result(channel->ssl);
        if (channel->tls_verify_result != X509_V_OK) {
            snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "TLS certificate verification failed: %s", X509_verify_cert_error_string(channel->tls_verify_result));
        } else {
            unsigned long err = ERR_get_error();
            if (err != 0) {
                ERR_error_string_n(err, channel->last_error_detail, sizeof(channel->last_error_detail));
            } else {
                snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "TLS handshake failed: SSL_get_error=%d", ssl_error);
            }
        }
        set_fd_nonblocking_mode(channel->fd, false);
        return -1;
    }
    set_fd_nonblocking_mode(channel->fd, false);
    channel->tls_verify_result = SSL_get_verify_result(channel->ssl);
    if (channel->tls_verify_result != X509_V_OK) {
        snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "TLS certificate verification failed: %s", X509_verify_cert_error_string(channel->tls_verify_result));
        return -1;
    }
    const unsigned char *selected_alpn = NULL;
    unsigned int selected_alpn_len = 0;
    SSL_get0_alpn_selected(channel->ssl, &selected_alpn, &selected_alpn_len);
    if (selected_alpn_len != 2 || selected_alpn == NULL || memcmp(selected_alpn, "h2", 2) != 0) {
        if (selected_alpn_len > 0 && selected_alpn != NULL) {
            size_t copy_len = selected_alpn_len < sizeof(channel->negotiated_protocol) - 1 ? selected_alpn_len : sizeof(channel->negotiated_protocol) - 1;
            memcpy(channel->negotiated_protocol, selected_alpn, copy_len);
            channel->negotiated_protocol[copy_len] = '\0';
            snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "TLS ALPN did not negotiate h2: %s", channel->negotiated_protocol);
        } else {
            set_channel_error_detail(channel, "TLS ALPN did not negotiate h2");
        }
        return -1;
    }
    snprintf(channel->negotiated_protocol, sizeof(channel->negotiated_protocol), "h2");
    channel->tls = true;
    return 0;
}

static ssize_t channel_send(grpc_call *client, const uint8_t *data, size_t length)
{
    zend_long remaining_timeout_us;
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(client->deadline_abs_us);
    if (remaining_timeout_us < 0) {
        errno = ETIMEDOUT;
        client->timed_out = true;
        return -1;
    }
    if (client->channel != NULL && set_socket_timeout_us(client->channel->fd, remaining_timeout_us) != 0) {
        return -1;
    }
    if (client->channel != NULL && client->channel->ssl != NULL) {
        int written = SSL_write(client->channel->ssl, data, (int) length);
        if (written <= 0) {
            int ssl_error = SSL_get_error(client->channel->ssl, written);
            client->last_ssl_error = ssl_error;
            client->channel->last_ssl_error = ssl_error;
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            client->last_io_errno = errno;
            client->channel->last_io_errno = errno;
            snprintf(client->last_io_error_detail, sizeof(client->last_io_error_detail), "SSL_write failed: SSL_get_error=%d", ssl_error);
            set_channel_error_detail(client->channel, client->last_io_error_detail);
            return -1;
        }
        return written;
    }
    ssize_t written = send(client->fd, data, length, 0);
    if (written < 0) {
        client->last_io_errno = errno;
        snprintf(client->last_io_error_detail, sizeof(client->last_io_error_detail), "send failed: %s", strerror(errno));
        if (client->channel != NULL) {
            client->channel->last_io_errno = errno;
            set_channel_error_detail(client->channel, client->last_io_error_detail);
        }
    }
    return written;
}

static ssize_t channel_recv(h2_channel *channel, uint8_t *data, size_t length, uint64_t deadline_abs_us)
{
    zend_long remaining_timeout_us;
    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (set_socket_timeout_us(channel->fd, remaining_timeout_us) != 0) {
        return -1;
    }
    if (channel != NULL && channel->ssl != NULL) {
        int nread = SSL_read(channel->ssl, data, (int) length);
        if (nread <= 0) {
            int ssl_error = SSL_get_error(channel->ssl, nread);
            channel->last_ssl_error = ssl_error;
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            channel->last_io_errno = errno;
            snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "SSL_read failed: SSL_get_error=%d", ssl_error);
            return -1;
        }
        return nread;
    }
    ssize_t nread = recv(channel->fd, data, length, 0);
    if (nread < 0 && channel != NULL) {
        channel->last_io_errno = errno;
        snprintf(channel->last_error_detail, sizeof(channel->last_error_detail), "recv failed: %s", strerror(errno));
    }
    return nread;
}

static h2_channel *create_h2_channel(const char *host, zend_long port, const char *authority, size_t authority_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message)
{
    h2_channel *channel;
    grpc_call open_client;
    int rv;

    channel = pecalloc(1, sizeof(h2_channel), persistent);
    channel->persistent = persistent;
    channel->fd = -1;
    channel->tls_verify_result = X509_V_OK;
    channel->fd = connect_tcp(host, port, deadline_abs_us);
    if (channel->fd < 0) {
        if (errno == ETIMEDOUT) {
            *error_message = "HTTP/2 transport deadline exceeded";
        } else {
            *error_message = "failed to connect";
        }
        pefree(channel, persistent);
        return NULL;
    }
    if (use_tls && configure_tls_channel(channel, host, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us) != 0) {
        if (channel->last_error_detail[0] != '\0') {
            snprintf(error_detail, error_detail_len, "%s", channel->last_error_detail);
            *error_message = error_detail;
        } else {
            *error_message = "failed to establish TLS";
        }
        destroy_h2_channel(channel);
        return NULL;
    }

    memset(&open_client, 0, sizeof(open_client));
    open_client.fd = channel->fd;
    open_client.channel = channel;
    open_client.grpc_status = -1;
    open_client.http_status = -1;

    if (configure_callbacks(&channel->callbacks) != 0) {
        destroy_h2_channel(channel);
        *error_message = "failed to configure callbacks";
        return NULL;
    }
    if (nghttp2_session_client_new(&channel->session, channel->callbacks, &open_client) != 0) {
        destroy_h2_channel(channel);
        *error_message = "failed to create nghttp2 session";
        return NULL;
    }

    nghttp2_submit_settings(channel->session, NGHTTP2_FLAG_NONE, NULL, 0);
    rv = nghttp2_session_send(channel->session);
    if (rv != 0) {
        destroy_h2_channel(channel);
        *error_message = "nghttp2_session_send failed";
        return NULL;
    }
    nghttp2_session_set_user_data(channel->session, NULL);

    if (authority != NULL && authority_len > 0) {
        size_t copy_len = authority_len < sizeof(channel->authority) - 1 ? authority_len : sizeof(channel->authority) - 1;
        memcpy(channel->authority, authority, copy_len);
        channel->authority[copy_len] = '\0';
    } else {
        snprintf(channel->authority, sizeof(channel->authority), "%s:%ld", host, port);
    }
    return channel;
}

static h2_channel *get_persistent_channel(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message)
{
    h2_channel *channel;

    *persistent_reused = false;
    if (!PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        *error_message = "persistent channel cache is not initialized";
        return NULL;
    }

    channel = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_channels), key, key_len);
    if (channel != NULL && !channel_usable(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
        channel = NULL;
    }
    if (channel != NULL && !preflight_persistent_channel(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
        channel = NULL;
    }

    if (channel == NULL) {
        channel = create_h2_channel(host, port, authority, authority_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, true, deadline_abs_us, error_detail, error_detail_len, error_message);
        if (channel == NULL) {
            return NULL;
        }
        zend_hash_str_update_ptr(&PHP_GRPC_LITE_G(persistent_channels), key, key_len, channel);
        return channel;
    }

    *persistent_reused = true;
    return channel;
}

static void discard_persistent_channel(const char *key, size_t key_len, h2_channel *channel)
{
    if (PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        zend_hash_str_del(&PHP_GRPC_LITE_G(persistent_channels), key, key_len);
    }
    if (channel == NULL) {
        return;
    }
    if (channel->busy) {
        channel->detached_from_cache = true;
        return;
    }
    destroy_h2_channel(channel);
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

static int set_nonblocking(int fd)
{
    return set_fd_nonblocking_mode(fd, true);
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    if (client == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    while (total_written < length) {
        ssize_t written = channel_send(client, data + total_written, length - total_written);
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static size_t remaining_request_bytes(grpc_call *client)
{
    size_t total_len = client->grpc_header_len + client->request_len;
    if (client->request_offset >= total_len) {
        return 0;
    }
    return total_len - client->request_offset;
}

static size_t copy_request_bytes(grpc_call *client, uint8_t *buf, size_t length)
{
    size_t copied = 0;
    size_t total_len = client->grpc_header_len + client->request_len;

    while (copied < length && client->request_offset < total_len) {
        if (client->request_offset < client->grpc_header_len) {
            size_t header_offset = client->request_offset;
            size_t remaining = client->grpc_header_len - header_offset;
            size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
            memcpy(buf + copied, client->grpc_header + header_offset, to_copy);
            copied += to_copy;
            client->request_offset += to_copy;
            continue;
        }

        size_t payload_offset = client->request_offset - client->grpc_header_len;
        size_t remaining = client->request_len - payload_offset;
        size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
        memcpy(buf + copied, client->request + payload_offset, to_copy);
        copied += to_copy;
        client->request_offset += to_copy;
    }

    return copied;
}

static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t remaining = remaining_request_bytes(client);
    size_t to_send = remaining < length ? remaining : length;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    client->data_read_calls++;

    size_t copied = copy_request_bytes(client, buf, to_send);
    if (client->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static void set_grpc_header(grpc_call *client, size_t payload_len)
{
    client->grpc_header[0] = 0;
    client->grpc_header[1] = (uint8_t) ((payload_len >> 24) & 0xff);
    client->grpc_header[2] = (uint8_t) ((payload_len >> 16) & 0xff);
    client->grpc_header[3] = (uint8_t) ((payload_len >> 8) & 0xff);
    client->grpc_header[4] = (uint8_t) (payload_len & 0xff);
    client->grpc_header_len = 5;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    (void) flags;
    if (stream_id == client->stream_id && len > 0) {
        client->data_recv_calls++;
        if (client->direct_response_payload && client->decode_response_incrementally && ((client->payload_callback_fci != NULL && client->payload_callback_fcc != NULL) || client->queue_response_payloads)) {
            if (process_response_data_direct(session, client, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        } else {
            if (validate_response_message_lengths(session, client, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (client->discard_response_body) {
                return 0;
            }
            smart_str_appendl(&client->body, (const char *) data, len);
        }
    }
    return 0;
}

static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    bool trailing;
    (void) session;
    (void) flags;
    if (frame->hd.stream_id != client->stream_id) {
        return 0;
    }
    trailing = frame->headers.cat != NGHTTP2_HCAT_RESPONSE;
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        client->grpc_status = parse_grpc_status_value(value, valuelen);
        if (client->grpc_status < 0) {
            client->invalid_grpc_status = true;
        }
        trailing = true;
    } else if (namelen == sizeof("grpc-message") - 1 && memcmp(name, "grpc-message", namelen) == 0) {
        if (client->grpc_message != NULL) {
            zend_string_release(client->grpc_message);
        }
        client->grpc_message = zend_string_init((const char *) value, valuelen, 0);
        trailing = true;
    } else if (namelen == sizeof("grpc-status-details-bin") - 1 && memcmp(name, "grpc-status-details-bin", namelen) == 0) {
        trailing = true;
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->http_status = atoi(status_buf);
    }
    if (add_metadata_entry(client, name, namelen, value, valuelen, trailing) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    (void) error_code;
    if (stream_id == client->stream_id) {
        client->stream_closed = true;
        client->stream_error_code = error_code;
    }
    return 0;
}

static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->sent_frames++;
    client->last_sent_frame_type = frame->hd.type;
    client->last_sent_frame_flags = frame->hd.flags;
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->recv_frames++;
    client->last_recv_frame_type = frame->hd.type;
    client->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        mark_channel_draining(client->channel, frame->goaway.last_stream_id, frame->goaway.error_code);
        if (client->stream_id > 0 && frame->goaway.last_stream_id < client->stream_id) {
            client->stream_error_code = NGHTTP2_REFUSED_STREAM;
            client->stream_closed = true;
        }
    }
    return 0;
}

static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->not_sent_frames++;
    client->last_not_sent_frame_type = frame->hd.type;
    client->last_not_sent_error = lib_error_code;
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

static int parse_grpc_status_value(const uint8_t *value, size_t valuelen)
{
    int status = 0;

    if (valuelen == 0 || valuelen > 2) {
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

static void init_request_headers(h2_request_headers *headers, size_t custom_values)
{
    headers->capacity = 7 + custom_values;
    headers->len = 0;
    headers->value_count = 0;
    headers->nva = ecalloc(headers->capacity == 0 ? 1 : headers->capacity, sizeof(nghttp2_nv));
    headers->value_strings = custom_values > 0 ? ecalloc(custom_values, sizeof(zend_string *)) : NULL;
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

static bool is_forbidden_custom_request_header(zend_string *key)
{
    const char *name;

    if (key == NULL || ZSTR_LEN(key) == 0) {
        return true;
    }
    name = ZSTR_VAL(key);
    if (name[0] == ':') {
        return true;
    }
    return false;
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
                zend_string *value_str = zval_get_string(nested);
                if (headers->value_strings != NULL) {
                    headers->value_strings[headers->value_count++] = value_str;
                }
                append_request_header(headers, ZSTR_VAL(key), ZSTR_LEN(key), ZSTR_VAL(value_str), ZSTR_LEN(value_str));
            } ZEND_HASH_FOREACH_END();
            continue;
        }

        zend_string *value_str = zval_get_string(value);
        if (headers->value_strings != NULL) {
            headers->value_strings[headers->value_count++] = value_str;
        }
        append_request_header(headers, ZSTR_VAL(key), ZSTR_LEN(key), ZSTR_VAL(value_str), ZSTR_LEN(value_str));
    } ZEND_HASH_FOREACH_END();

    return 0;
}

static void free_request_headers(h2_request_headers *headers)
{
    size_t i;
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
    headers->value_strings = NULL;
    headers->len = 0;
    headers->capacity = 0;
    headers->value_count = 0;
}

static int validate_response_message_lengths(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len && !client->response_message_too_large) {
        if (client->response_header_len < 5) {
            size_t need = 5 - client->response_header_len;
            size_t take = need < len - offset ? need : len - offset;
            memcpy(client->response_header_buf + client->response_header_len, data + offset, take);
            client->response_header_len += take;
            offset += take;
            if (client->response_header_len < 5) {
                continue;
            }

            client->response_payload_len = ((uint32_t) client->response_header_buf[1] << 24)
                | ((uint32_t) client->response_header_buf[2] << 16)
                | ((uint32_t) client->response_header_buf[3] << 8)
                | (uint32_t) client->response_header_buf[4];
            if (client->response_header_buf[0] != 0) {
                client->compressed_response_seen = true;
            }
            client->response_payload_offset = 0;
            if ((size_t) client->response_payload_len > client->max_receive_message_bytes) {
                client->response_message_too_large = true;
                client->discard_response_body = true;
                if (session != NULL && client->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, client->stream_id, NGHTTP2_CANCEL);
                }
                return 0;
            }
            if (client->response_payload_len == 0) {
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
                continue;
            }
        }

        if (client->response_payload_len > 0) {
            size_t need = client->response_payload_len - client->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            client->response_payload_offset += take;
            offset += take;
            if (client->response_payload_offset == client->response_payload_len) {
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
            }
        }
    }

    return 0;
}

static int process_response_data_direct(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len && !client->response_message_too_large && !client->compressed_response_seen) {
        if (client->response_header_len < 5) {
            size_t need = 5 - client->response_header_len;
            size_t take = need < len - offset ? need : len - offset;
            memcpy(client->response_header_buf + client->response_header_len, data + offset, take);
            client->response_header_len += take;
            offset += take;
            if (client->response_header_len < 5) {
                continue;
            }

            client->response_payload_len = ((uint32_t) client->response_header_buf[1] << 24)
                | ((uint32_t) client->response_header_buf[2] << 16)
                | ((uint32_t) client->response_header_buf[3] << 8)
                | (uint32_t) client->response_header_buf[4];
            if ((size_t) client->response_payload_len > client->max_receive_message_bytes) {
                client->response_message_too_large = true;
                client->discard_response_body = true;
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
                client->response_current_compressed = false;
                if (session != NULL && client->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, client->stream_id, NGHTTP2_CANCEL);
                }
                client->response_payload_offset = 0;
                continue;
            }
            client->response_current_compressed = client->response_header_buf[0] != 0;
            if (client->response_header_buf[0] != 0) {
                client->compressed_response_seen = true;
                client->discard_response_body = true;
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
                client->response_current_compressed = false;
                if (session != NULL && client->stream_id > 0) {
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, client->stream_id, NGHTTP2_CANCEL);
                }
                continue;
            }
            client->response_payload_offset = 0;
            if (client->response_current_compressed) {
                if (client->response_payload_len == 0) {
                    client->response_header_len = 0;
                    client->response_payload_len = 0;
                    client->response_payload_offset = 0;
                    client->response_current_compressed = false;
                }
            } else {
                client->response_payload = zend_string_alloc(client->response_payload_len, 0);
                if (client->response_payload_len == 0) {
                    ZSTR_VAL(client->response_payload)[0] = '\0';
                }
            }
        }

        if (client->response_current_compressed) {
            size_t need = client->response_payload_len - client->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            client->response_payload_offset += take;
            offset += take;
            if (client->response_payload_offset == client->response_payload_len) {
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
                client->response_current_compressed = false;
            }
        } else if (client->response_payload != NULL) {
            size_t need = client->response_payload_len - client->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            uint64_t payload_copy_started = client->observe_payload_copy != NULL ? monotonic_us() : 0;
            if (take > 0) {
                memcpy(ZSTR_VAL(client->response_payload) + client->response_payload_offset, data + offset, take);
                client->response_payload_offset += take;
                offset += take;
            }
            if (client->observe_payload_copy != NULL) {
                client->observe_payload_copy(client, monotonic_us() - payload_copy_started);
            }

            if (client->response_payload_offset == client->response_payload_len) {
                zend_string *payload = client->response_payload;
                uint64_t ready_abs_us = monotonic_us();

                if (client->observe_message_ready != NULL) {
                    client->observe_message_ready(client, ready_abs_us);
                }
                ZSTR_VAL(payload)[client->response_payload_len] = '\0';
                client->response_payload = NULL;
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;

                if (client->queue_response_payloads) {
                    if (enqueue_response_payload(client, payload) != 0) {
                        return -1;
                    }
                } else if (deliver_response_payload(client, payload, ready_abs_us) != 0) {
                    zend_string_release(payload);
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int enqueue_response_payload(grpc_call *client, zend_string *payload)
{
    queued_payload *entry = emalloc(sizeof(queued_payload));
    entry->payload = payload;
    entry->ready_abs_us = monotonic_us();
    entry->next = NULL;

    if (client->response_queue_tail == NULL) {
        client->response_queue_head = entry;
    } else {
        client->response_queue_tail->next = entry;
    }
    client->response_queue_tail = entry;
    client->response_queue_count++;
    client->response_queue_bytes += ZSTR_LEN(payload);
    if (client->observe_payload_queued != NULL) {
        client->observe_payload_queued(client);
    }

    if (client->flush_queue_if_limited != NULL && client->flush_queue_if_limited(client) != 0) {
        return -1;
    }

    return 0;
}

static int deliver_response_payload(grpc_call *client, zend_string *payload, uint64_t ready_abs_us)
{
    zval params[1];
    zval retval;
    uint64_t started = client->observe_payload_delivered != NULL ? monotonic_us() : 0;

    ZVAL_STR(&params[0], payload);
    ZVAL_UNDEF(&retval);
    client->payload_callback_fci->params = params;
    client->payload_callback_fci->param_count = 1;
    client->payload_callback_fci->retval = &retval;

    if (zend_call_function(client->payload_callback_fci, client->payload_callback_fcc) != SUCCESS || EG(exception)) {
        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        return -1;
    }
    if (client->observe_payload_delivered != NULL) {
        uint64_t elapsed = monotonic_us() - started;
        client->observe_payload_delivered(client, ready_abs_us, started, elapsed);
    }

    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
    }

    return 0;
}

static int deliver_queued_response_payloads(grpc_call *client)
{
    while (client->response_queue_head != NULL) {
        queued_payload *entry = client->response_queue_head;
        client->response_queue_head = entry->next;
        if (client->response_queue_head == NULL) {
            client->response_queue_tail = NULL;
        }
        client->response_queue_count--;
        client->response_queue_bytes -= ZSTR_LEN(entry->payload);
        if (deliver_response_payload(client, entry->payload, entry->ready_abs_us) != 0) {
            efree(entry);
            return -1;
        }
        efree(entry);
    }

    return 0;
}

static void free_queued_response_payloads(grpc_call *client)
{
    while (client->response_queue_head != NULL) {
        queued_payload *entry = client->response_queue_head;
        client->response_queue_head = entry->next;
        zend_string_release(entry->payload);
        efree(entry);
    }
    client->response_queue_tail = NULL;
    client->response_queue_count = 0;
    client->response_queue_bytes = 0;
}

static int add_metadata_entry(grpc_call *client, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing)
{
    metadata_entry *entry;
    size_t entry_bytes = namelen + valuelen;

    if (namelen == 0 || name[0] == ':') {
        return 0;
    }
    if (client->metadata_too_large
        || client->metadata_entry_count >= GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES
        || entry_bytes > GRPC_LITE_MAX_RESPONSE_METADATA_BYTES
        || client->metadata_bytes > GRPC_LITE_MAX_RESPONSE_METADATA_BYTES - entry_bytes) {
        client->metadata_too_large = true;
        return 0;
    }

    entry = emalloc(sizeof(metadata_entry));
    entry->key = zend_string_init((const char *) name, namelen, 0);
    entry->value = zend_string_init((const char *) value, valuelen, 0);
    entry->trailing = trailing;
    entry->next = NULL;

    if (client->metadata_tail == NULL) {
        client->metadata_head = entry;
    } else {
        client->metadata_tail->next = entry;
    }
    client->metadata_tail = entry;
    client->metadata_entry_count++;
    client->metadata_bytes += entry_bytes;

    return 0;
}

static void free_metadata_entries(grpc_call *client)
{
    while (client->metadata_head != NULL) {
        metadata_entry *entry = client->metadata_head;
        client->metadata_head = entry->next;
        zend_string_release(entry->key);
        zend_string_release(entry->value);
        efree(entry);
    }
    client->metadata_tail = NULL;
}

static void add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *client, bool trailing)
{
    zval metadata;
    metadata_entry *entry;

    array_init(&metadata);
    for (entry = client->metadata_head; entry != NULL; entry = entry->next) {
        zval *values;

        if (entry->trailing != trailing) {
            continue;
        }
        values = zend_hash_find(Z_ARRVAL(metadata), entry->key);
        if (values == NULL) {
            zval new_values;
            array_init(&new_values);
            add_next_index_str(&new_values, zend_string_copy(entry->value));
            zend_hash_update(Z_ARRVAL(metadata), entry->key, &new_values);
        } else if (Z_TYPE_P(values) == IS_ARRAY) {
            add_next_index_str(values, zend_string_copy(entry->value));
        }
    }

    add_assoc_zval(return_value, name, &metadata);
}

static void cleanup_grpc_call(grpc_call *client)
{
    if (client == NULL) {
        return;
    }
    if (client->response_payload != NULL) {
        zend_string_release(client->response_payload);
        client->response_payload = NULL;
    }
    free_queued_response_payloads(client);
    free_metadata_entries(client);
    if (client->grpc_message != NULL) {
        zend_string_release(client->grpc_message);
        client->grpc_message = NULL;
    }
    smart_str_free(&client->body);
}
