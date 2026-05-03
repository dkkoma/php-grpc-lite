#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp2/nghttp2.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAKE_NV(NAME, VALUE) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE}
#define MAKE_NV_L(NAME, VALUE, VALUE_LEN) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, (VALUE_LEN), NGHTTP2_NV_FLAG_NONE}
#define MAX_RECV_BUF_SIZE 262144

typedef struct _poc_channel poc_channel;
typedef struct _poc_stream poc_stream;

typedef struct queued_payload {
    zend_string *payload;
    uint64_t ready_abs_us;
    struct queued_payload *next;
} queued_payload;

typedef struct queued_raw_payload {
    char *payload;
    size_t len;
    uint64_t ready_abs_us;
    struct queued_raw_payload *next;
} queued_raw_payload;

typedef struct {
    int fd;
    poc_channel *channel;
    int32_t stream_id;
    bool stream_closed;
    int grpc_status;
    uint32_t stream_error_code;
    int http_status;
    size_t bytes_sent;
    size_t bytes_received;
    size_t data_read_calls;
    size_t data_recv_calls;
    size_t data_read_length_calls;
    int last_session_error;
    int last_sent_frame_type;
    int last_recv_frame_type;
    int last_sent_frame_flags;
    int last_recv_frame_flags;
    int last_not_sent_frame_type;
    int last_not_sent_error;
    size_t sent_frames;
    size_t recv_frames;
    size_t not_sent_frames;
    size_t data_frames_sent;
    size_t data_bytes_sent;
    size_t window_update_frames_recv;
    size_t connection_window_update_frames_recv;
    size_t stream_window_update_frames_recv;
    size_t connection_window_update_increment_recv;
    size_t stream_window_update_increment_recv;
    size_t window_update_frames_sent;
    size_t connection_window_update_frames_sent;
    size_t stream_window_update_frames_sent;
    size_t connection_window_update_increment_sent;
    size_t stream_window_update_increment_sent;
    size_t flow_control_pauses;
    size_t send_callback_calls;
    size_t send_data_callback_calls;
    size_t write_syscalls;
    size_t send_wouldblock_calls;
    size_t recv_wouldblock_calls;
    size_t poll_calls;
    size_t poll_timeouts;
    size_t poll_errors;
    bool timed_out;
    uint64_t deadline_abs_us;
    uint64_t max_write_syscall_us;
    size_t max_send_callback_len;
    size_t max_data_frame_len;
    size_t min_data_frame_len;
    size_t max_read_len;
    size_t min_read_len;
    uint32_t data_frame_size_cap;
    uint32_t recv_stream_window_size;
    uint32_t recv_connection_window_size;
    bool flush_after_mem_recv;
    bool read_first_poll_loop;
    int32_t min_session_remote_window;
    int32_t min_stream_remote_window;
    uint32_t remote_max_frame_size;
    bool no_copy;
    bool poll_loop;
    bool discard_response_body;
    bool last_send_wouldblock;
    uint64_t call_started_us;
    uint64_t first_data_sent_us;
    uint64_t last_data_sent_us;
    uint64_t first_response_data_us;
    uint64_t last_response_data_us;
    uint64_t first_window_update_us;
    uint64_t last_window_update_us;
    uint64_t first_window_update_sent_us;
    uint64_t last_window_update_sent_us;
    uint64_t last_window_update_sent_abs_us;
    uint64_t last_poll_return_abs_us;
    bool awaiting_data_after_poll;
    bool awaiting_data_after_window_update_sent;
    uint64_t first_flow_control_pause_us;
    uint64_t first_response_header_us;
    uint64_t stream_closed_us;
    uint64_t first_response_message_ready_us;
    uint64_t last_response_message_ready_us;
    uint64_t first_response_callback_done_us;
    uint64_t last_response_callback_done_us;
    size_t response_data_bytes;
    size_t call_response_data_bytes;
    size_t call_data_recv_calls;
    uint64_t call_body_append_us;
    uint64_t call_max_body_append_us;
    zend_fcall_info *response_fci;
    zend_fcall_info_cache *response_fcc;
    bool decode_response_incrementally;
    bool direct_response_payload;
    bool read_ahead_delivery;
    size_t read_ahead_max_messages;
    size_t read_ahead_max_bytes;
    bool transport_thread;
    bool queue_response_payloads;
    queued_payload *response_queue_head;
    queued_payload *response_queue_tail;
    size_t response_queue_count;
    size_t response_queue_bytes;
    size_t call_max_response_queue_count;
    size_t call_max_response_queue_bytes;
    uint64_t call_response_queue_wait_us;
    uint64_t call_max_response_queue_wait_us;
    queued_raw_payload *raw_response_queue_head;
    queued_raw_payload *raw_response_queue_tail;
    char *response_raw_payload;
    bool compact_response_buffer;
    size_t response_compact_threshold;
    size_t response_parse_offset;
    uint8_t response_header_buf[5];
    size_t response_header_len;
    uint32_t response_payload_len;
    size_t response_payload_offset;
    zend_string *response_payload;
    zend_long call_decoded_messages;
    uint64_t call_response_payload_string_us;
    uint64_t call_max_response_payload_string_us;
    uint64_t call_response_decode_us;
    uint64_t call_max_response_decode_us;
    size_t call_body_compact_count;
    size_t call_body_compact_bytes;
    uint64_t call_body_compact_us;
    uint64_t call_max_body_compact_us;
    size_t call_max_body_buffer_bytes;
    size_t call_window_update_frames_recv;
    size_t call_connection_window_update_frames_recv;
    size_t call_stream_window_update_frames_recv;
    size_t call_connection_window_update_increment_recv;
    size_t call_stream_window_update_increment_recv;
    size_t call_window_update_frames_sent;
    size_t call_connection_window_update_frames_sent;
    size_t call_stream_window_update_frames_sent;
    size_t call_connection_window_update_increment_sent;
    size_t call_stream_window_update_increment_sent;
    size_t call_data_read_length_calls;
    size_t call_flow_control_pauses;
    uint64_t call_max_write_syscall_us;
    size_t call_recv_syscalls;
    uint64_t call_recv_syscall_us;
    uint64_t call_max_recv_syscall_us;
    uint64_t call_mem_recv_us;
    uint64_t call_max_mem_recv_us;
    uint64_t call_session_send_after_recv_us;
    uint64_t call_max_session_send_after_recv_us;
    uint64_t call_poll_wait_us;
    uint64_t call_max_poll_wait_us;
    size_t call_pollin_ready;
    size_t call_pollout_ready;
    uint64_t call_poll_to_data_us;
    uint64_t call_max_poll_to_data_us;
    uint64_t call_window_update_to_data_us;
    uint64_t call_max_window_update_to_data_us;
    size_t call_receive_drains;
    size_t call_receive_drains_with_data;
    size_t call_receive_drains_eagain_after_data;
    size_t call_max_reads_per_drain;
    size_t call_max_bytes_per_drain;
    int32_t call_min_session_remote_window;
    int32_t call_min_stream_remote_window;
    zend_long server_handler_ns;
    zend_long server_payload_alloc_ns;
    zend_long server_payload_bytes;
    zend_long server_request_payload_bytes;
    zend_long server_stats_handler_start_ns;
    zend_long server_stats_handler_end_ns;
    zend_long server_stats_in_payload_ns;
    zend_long server_stats_out_header_ns;
    zend_long server_stats_out_payload_ns;
    zend_long server_stats_first_out_payload_ns;
    zend_long server_stats_last_out_payload_ns;
    zend_long server_stats_out_payload_count;
    zend_long server_stats_out_payload_bytes;
    zend_long server_stats_out_payload_wire_bytes;
    zend_long server_stats_out_payload_compressed_bytes;
    smart_str body;
    uint8_t grpc_header[5];
    size_t grpc_header_len;
    const uint8_t *request;
    size_t request_len;
    size_t request_offset;
    size_t pending_data_len;
    struct iovec pending_write_iov[4];
    size_t pending_write_iovcnt;
    size_t pending_write_remaining;
    size_t pending_write_payload_len;
} poc_client;

static uint64_t monotonic_us(void);
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
static void record_data_sent(poc_client *client);
static int process_response_messages(poc_client *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long *decoded_messages, uint64_t *decode_us, uint64_t *max_decode_us);
static int process_response_messages_from_offset(poc_client *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us);
static int process_response_data_direct(poc_client *client, const uint8_t *data, size_t len);
static int enqueue_response_payload(poc_client *client, zend_string *payload);
static int deliver_response_payload(poc_client *client, zend_string *payload, uint64_t ready_abs_us);
static int deliver_queued_response_payloads(poc_client *client);
static int deliver_queued_response_payloads_if_bounded(poc_client *client);
static void free_queued_response_payloads(poc_client *client);
static int enqueue_raw_response_payload(poc_client *client, char *payload, size_t len, uint64_t ready_abs_us);
static int deliver_raw_response_payload(poc_client *client, char *payload, size_t len, uint64_t ready_abs_us);
static int deliver_queued_raw_response_payloads(poc_client *client);
static void free_queued_raw_response_payloads(poc_client *client);
static void compact_response_body_if_needed(poc_client *client);
static bool channel_usable(poc_channel *channel);
static int connect_tcp(const char *host, zend_long port);
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);
static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data);

typedef struct {
    int32_t stream_id;
    bool stream_closed;
    int grpc_status;
    uint32_t stream_error_code;
    const uint8_t *request;
    size_t request_len;
    size_t request_offset;
    smart_str body;
} mux_stream;

typedef struct {
    int fd;
    mux_stream *streams;
    size_t stream_count;
    size_t closed_count;
    size_t bytes_sent;
    size_t bytes_received;
} mux_context;

static ssize_t mux_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static ssize_t mux_data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
static int mux_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
static int mux_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int mux_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);

typedef struct {
    nghttp2_session *session;
    poc_client *client;
    size_t recv_buf_len;
    int rv;
} transport_thread_args;

static void *transport_thread_main(void *arg);

struct _poc_channel {
    int fd;
    bool tls;
    bool persistent;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    char authority[512];
    bool dead;
    bool draining;
    bool busy;
    int last_error;
    uint32_t last_goaway_error_code;
    int32_t last_goaway_stream_id;
};

struct _poc_stream {
    poc_channel *channel;
    poc_client client;
    zend_string *request;
    char *recv_buf;
    size_t recv_buf_len;
    bool completed;
    bool cancelled;
};

static int le_poc_channel;
static int le_poc_stream;

ZEND_BEGIN_MODULE_GLOBALS(nghttp2_poc)
    HashTable persistent_channels;
    bool persistent_channels_initialized;
ZEND_END_MODULE_GLOBALS(nghttp2_poc)

ZEND_DECLARE_MODULE_GLOBALS(nghttp2_poc)

#define NGHTTP2_POC_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(nghttp2_poc, v)

static void destroy_poc_channel(poc_channel *channel)
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

static void poc_channel_dtor(zend_resource *rsrc)
{
    destroy_poc_channel((poc_channel *) rsrc->ptr);
}

static void destroy_poc_stream(poc_stream *stream)
{
    if (stream == NULL) {
        return;
    }
    if (!stream->completed && stream->channel != NULL && channel_usable(stream->channel) && stream->client.stream_id > 0) {
        nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        nghttp2_session_send(stream->channel->session);
    }
    if (stream->channel != NULL) {
        nghttp2_session_set_user_data(stream->channel->session, NULL);
        stream->channel->busy = false;
    }
    if (stream->request != NULL) {
        zend_string_release(stream->request);
    }
    if (stream->recv_buf != NULL) {
        efree(stream->recv_buf);
    }
    if (stream->client.response_payload != NULL) {
        zend_string_release(stream->client.response_payload);
        stream->client.response_payload = NULL;
    }
    free_queued_response_payloads(&stream->client);
    free_queued_raw_response_payloads(&stream->client);
    if (stream->client.response_raw_payload != NULL) {
        free(stream->client.response_raw_payload);
    }
    smart_str_free(&stream->client.body);
    efree(stream);
}

static void poc_stream_dtor(zend_resource *rsrc)
{
    destroy_poc_stream((poc_stream *) rsrc->ptr);
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

static void mark_channel_dead(poc_channel *channel, int error_code)
{
    if (channel == NULL) {
        return;
    }
    channel->dead = true;
    channel->last_error = error_code;
}

static void mark_channel_draining(poc_channel *channel, int32_t last_stream_id, uint32_t error_code)
{
    if (channel == NULL) {
        return;
    }
    channel->draining = true;
    channel->last_goaway_stream_id = last_stream_id;
    channel->last_goaway_error_code = error_code;
}

static bool channel_usable(poc_channel *channel)
{
    return channel != NULL && channel->fd >= 0 && channel->session != NULL && !channel->dead && !channel->draining;
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
    if (cert_bio == NULL || key_bio == NULL) {
        if (cert_bio != NULL) {
            BIO_free(cert_bio);
        }
        if (key_bio != NULL) {
            BIO_free(key_bio);
        }
        return -1;
    }

    X509 *x509 = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(cert_bio);
    BIO_free(key_bio);
    if (x509 == NULL || pkey == NULL) {
        if (x509 != NULL) {
            X509_free(x509);
        }
        if (pkey != NULL) {
            EVP_PKEY_free(pkey);
        }
        return -1;
    }

    int ok = SSL_CTX_use_certificate(ctx, x509) == 1
        && SSL_CTX_use_PrivateKey(ctx, pkey) == 1
        && SSL_CTX_check_private_key(ctx) == 1;
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok ? 0 : -1;
}

static int configure_tls_channel(poc_channel *channel, const char *host, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len)
{
    channel->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (channel->ssl_ctx == NULL) {
        return -1;
    }
    SSL_CTX_set_verify(channel->ssl_ctx, SSL_VERIFY_PEER, NULL);

    if (root_certs != NULL && root_certs_len > 0) {
        if (add_pem_certs_to_store(SSL_CTX_get_cert_store(channel->ssl_ctx), root_certs, root_certs_len) != 0) {
            return -1;
        }
    } else if (SSL_CTX_set_default_verify_paths(channel->ssl_ctx) != 1) {
        return -1;
    }

    if (cert_chain != NULL && private_key != NULL && cert_chain_len > 0 && private_key_len > 0) {
        if (configure_client_certificate(channel->ssl_ctx, cert_chain, cert_chain_len, private_key, private_key_len) != 0) {
            return -1;
        }
    }

    channel->ssl = SSL_new(channel->ssl_ctx);
    if (channel->ssl == NULL) {
        return -1;
    }
    static const unsigned char alpn[] = {2, 'h', '2'};
    SSL_set_alpn_protos(channel->ssl, alpn, sizeof(alpn));
    SSL_set_tlsext_host_name(channel->ssl, host);
    SSL_set1_host(channel->ssl, host);
    SSL_set_fd(channel->ssl, channel->fd);
    if (SSL_connect(channel->ssl) != 1) {
        return -1;
    }
    channel->tls = true;
    return 0;
}

static ssize_t channel_send(poc_client *client, const uint8_t *data, size_t length)
{
    if (client->channel != NULL && client->channel->ssl != NULL) {
        int written = SSL_write(client->channel->ssl, data, (int) length);
        if (written <= 0) {
            int ssl_error = SSL_get_error(client->channel->ssl, written);
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            return -1;
        }
        return written;
    }
    return send(client->fd, data, length, 0);
}

static ssize_t channel_recv(poc_channel *channel, uint8_t *data, size_t length)
{
    if (channel != NULL && channel->ssl != NULL) {
        int nread = SSL_read(channel->ssl, data, (int) length);
        if (nread <= 0) {
            int ssl_error = SSL_get_error(channel->ssl, nread);
            errno = (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
            return -1;
        }
        return nread;
    }
    return recv(channel->fd, data, length, 0);
}

static poc_channel *create_poc_channel(const char *host, zend_long port, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, const char **error_message)
{
    poc_channel *channel;
    poc_client open_client;
    int rv;

    channel = pecalloc(1, sizeof(poc_channel), persistent);
    channel->persistent = persistent;
    channel->fd = -1;
    channel->fd = connect_tcp(host, port);
    if (channel->fd < 0) {
        pefree(channel, persistent);
        *error_message = "failed to connect";
        return NULL;
    }
    if (use_tls && configure_tls_channel(channel, host, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len) != 0) {
        destroy_poc_channel(channel);
        *error_message = "failed to establish TLS";
        return NULL;
    }

    memset(&open_client, 0, sizeof(open_client));
    open_client.fd = channel->fd;
    open_client.channel = channel;
    open_client.grpc_status = -1;
    open_client.http_status = -1;

    if (configure_callbacks(&channel->callbacks) != 0) {
        destroy_poc_channel(channel);
        *error_message = "failed to configure callbacks";
        return NULL;
    }
    if (nghttp2_session_client_new(&channel->session, channel->callbacks, &open_client) != 0) {
        destroy_poc_channel(channel);
        *error_message = "failed to create nghttp2 session";
        return NULL;
    }

    nghttp2_submit_settings(channel->session, NGHTTP2_FLAG_NONE, NULL, 0);
    rv = nghttp2_session_send(channel->session);
    if (rv != 0) {
        destroy_poc_channel(channel);
        *error_message = "nghttp2_session_send failed";
        return NULL;
    }
    nghttp2_session_set_user_data(channel->session, NULL);

    snprintf(channel->authority, sizeof(channel->authority), "%s:%ld", host, port);
    return channel;
}

static poc_channel *get_persistent_channel(const char *key, size_t key_len, const char *host, zend_long port, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool *persistent_reused, const char **error_message)
{
    poc_channel *channel;

    *persistent_reused = false;
    if (!NGHTTP2_POC_G(persistent_channels_initialized)) {
        *error_message = "persistent channel cache is not initialized";
        return NULL;
    }

    channel = zend_hash_str_find_ptr(&NGHTTP2_POC_G(persistent_channels), key, key_len);
    if (channel != NULL && !channel_usable(channel)) {
        destroy_poc_channel(channel);
        zend_hash_str_del(&NGHTTP2_POC_G(persistent_channels), key, key_len);
        channel = NULL;
    }

    if (channel == NULL) {
        channel = create_poc_channel(host, port, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, true, error_message);
        if (channel == NULL) {
            return NULL;
        }
        zend_hash_str_update_ptr(&NGHTTP2_POC_G(persistent_channels), key, key_len, channel);
        return channel;
    }

    *persistent_reused = true;
    return channel;
}

static void discard_persistent_channel(const char *key, size_t key_len, poc_channel *channel)
{
    if (channel != NULL) {
        destroy_poc_channel(channel);
    }
    if (NGHTTP2_POC_G(persistent_channels_initialized)) {
        zend_hash_str_del(&NGHTTP2_POC_G(persistent_channels), key, key_len);
    }
}

static int connect_tcp(const char *host, zend_long port)
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

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    client->send_callback_calls++;
    if (length > client->max_send_callback_len) {
        client->max_send_callback_len = length;
    }

    if (client->poll_loop) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = channel_send(client, data, length);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                client->last_send_wouldblock = true;
                client->send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            client->last_send_wouldblock = true;
            client->send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        client->write_syscalls++;
        client->bytes_sent += (size_t) written;
        return written;
    }

    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = channel_send(client, data + total_written, length - total_written);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        client->write_syscalls++;
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static size_t remaining_request_bytes(poc_client *client)
{
    size_t total_len = client->grpc_header_len + client->request_len;
    if (client->request_offset >= total_len) {
        return 0;
    }
    return total_len - client->request_offset;
}

static size_t copy_request_bytes(poc_client *client, uint8_t *buf, size_t length)
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
    poc_client *client = (poc_client *) user_data;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t remaining = remaining_request_bytes(client);
    size_t to_send = remaining < length ? remaining : length;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    client->data_read_calls++;

    if (client->no_copy && to_send > 0) {
        client->pending_data_len = to_send;
        *data_flags = NGHTTP2_DATA_FLAG_NO_COPY;
        if (client->request_offset + to_send >= total_len) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return (ssize_t) to_send;
    }

    size_t copied = copy_request_bytes(client, buf, to_send);
    if (client->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static ssize_t data_source_read_length_callback(nghttp2_session *session, uint8_t frame_type, int32_t stream_id, int32_t session_remote_window_size, int32_t stream_remote_window_size, uint32_t remote_max_frame_size, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t allowed = (size_t) session_remote_window_size;
    (void) session;
    (void) frame_type;
    (void) stream_id;

    if (stream_remote_window_size < session_remote_window_size) {
        allowed = (size_t) stream_remote_window_size;
    }
    if (remote_max_frame_size < allowed) {
        allowed = remote_max_frame_size;
    }
    if (client->data_frame_size_cap > 0 && client->data_frame_size_cap < allowed) {
        allowed = client->data_frame_size_cap;
    }
    if (allowed == 0) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->flow_control_pauses++;
        client->call_flow_control_pauses++;
        if (elapsed > 0 && client->first_flow_control_pause_us == 0) {
            client->first_flow_control_pause_us = elapsed;
        }
        return NGHTTP2_ERR_PAUSE;
    }

    client->data_read_length_calls++;
    client->call_data_read_length_calls++;
    client->remote_max_frame_size = remote_max_frame_size;
    if (client->min_session_remote_window == 0 || session_remote_window_size < client->min_session_remote_window) {
        client->min_session_remote_window = session_remote_window_size;
    }
    if (client->min_stream_remote_window == 0 || stream_remote_window_size < client->min_stream_remote_window) {
        client->min_stream_remote_window = stream_remote_window_size;
    }
    if (client->call_min_session_remote_window == 0 || session_remote_window_size < client->call_min_session_remote_window) {
        client->call_min_session_remote_window = session_remote_window_size;
    }
    if (client->call_min_stream_remote_window == 0 || stream_remote_window_size < client->call_min_stream_remote_window) {
        client->call_min_stream_remote_window = stream_remote_window_size;
    }
    if (allowed > client->max_read_len) {
        client->max_read_len = allowed;
    }
    if (client->min_read_len == 0 || allowed < client->min_read_len) {
        client->min_read_len = allowed;
    }

    return (ssize_t) allowed;
}

static void set_grpc_header(poc_client *client, size_t payload_len)
{
    client->grpc_header[0] = 0;
    client->grpc_header[1] = (uint8_t) ((payload_len >> 24) & 0xff);
    client->grpc_header[2] = (uint8_t) ((payload_len >> 16) & 0xff);
    client->grpc_header[3] = (uint8_t) ((payload_len >> 8) & 0xff);
    client->grpc_header[4] = (uint8_t) (payload_len & 0xff);
    client->grpc_header_len = 5;
}

static int write_all(poc_client *client, const uint8_t *data, size_t length)
{
    size_t total_written = 0;
    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = send(client->fd, data + total_written, length - total_written, 0);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        client->write_syscalls++;
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return 0;
}

static size_t fill_request_iov(poc_client *client, struct iovec *iov, size_t iov_offset, size_t length, size_t *filled_len)
{
    size_t filled = 0;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t request_offset = client->request_offset;

    while (filled < length && request_offset < total_len && iov_offset < 4) {
        if (request_offset < client->grpc_header_len) {
            size_t header_offset = request_offset;
            size_t remaining = client->grpc_header_len - header_offset;
            size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
            iov[iov_offset].iov_base = client->grpc_header + header_offset;
            iov[iov_offset].iov_len = to_write;
            iov_offset++;
            filled += to_write;
            request_offset += to_write;
            continue;
        }

        size_t payload_offset = request_offset - client->grpc_header_len;
        size_t remaining = client->request_len - payload_offset;
        size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
        iov[iov_offset].iov_base = (void *) (client->request + payload_offset);
        iov[iov_offset].iov_len = to_write;
        iov_offset++;
        filled += to_write;
        request_offset += to_write;
    }

    *filled_len = filled;
    return iov_offset;
}

static int write_data_frame(poc_client *client, const uint8_t *framehd, size_t length)
{
    struct iovec iov[4];
    size_t iovcnt = 1;
    size_t filled_len = 0;
    size_t total_len = 9 + length;
    size_t total_written = 0;

    iov[0].iov_base = (void *) framehd;
    iov[0].iov_len = 9;
    iovcnt = fill_request_iov(client, iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        return -1;
    }

    while (total_written < total_len) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(client->fd, iov, (int) iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        client->write_syscalls++;
        total_written += (size_t) written;

        size_t consumed = (size_t) written;
        for (size_t i = 0; i < iovcnt && consumed > 0; i++) {
            if (consumed >= iov[i].iov_len) {
                consumed -= iov[i].iov_len;
                iov[i].iov_base = (uint8_t *) iov[i].iov_base + iov[i].iov_len;
                iov[i].iov_len = 0;
                continue;
            }
            iov[i].iov_base = (uint8_t *) iov[i].iov_base + consumed;
            iov[i].iov_len -= consumed;
            consumed = 0;
        }
        while (iovcnt > 0 && iov[0].iov_len == 0) {
            memmove(&iov[0], &iov[1], sizeof(struct iovec) * (iovcnt - 1));
            iovcnt--;
        }
    }

    client->request_offset += length;
    client->bytes_sent += total_len;
    return 0;
}

static void clear_pending_write(poc_client *client)
{
    client->pending_write_iovcnt = 0;
    client->pending_write_remaining = 0;
    client->pending_write_payload_len = 0;
}

static void consume_pending_write(poc_client *client, size_t consumed)
{
    while (client->pending_write_iovcnt > 0 && consumed > 0) {
        struct iovec *iov = &client->pending_write_iov[0];
        if (consumed >= iov->iov_len) {
            consumed -= iov->iov_len;
            client->pending_write_remaining -= iov->iov_len;
            memmove(&client->pending_write_iov[0], &client->pending_write_iov[1], sizeof(struct iovec) * (client->pending_write_iovcnt - 1));
            client->pending_write_iovcnt--;
            continue;
        }

        iov->iov_base = (uint8_t *) iov->iov_base + consumed;
        iov->iov_len -= consumed;
        client->pending_write_remaining -= consumed;
        consumed = 0;
    }
}

static int prepare_pending_data_frame_write(poc_client *client, const uint8_t *framehd, size_t length)
{
    size_t iovcnt = 1;
    size_t filled_len = 0;

    client->pending_write_iov[0].iov_base = (void *) framehd;
    client->pending_write_iov[0].iov_len = 9;
    iovcnt = fill_request_iov(client, client->pending_write_iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        clear_pending_write(client);
        return -1;
    }

    client->pending_write_iovcnt = iovcnt;
    client->pending_write_remaining = 9 + length;
    client->pending_write_payload_len = length;
    return 0;
}

static int flush_pending_data_frame_write(poc_client *client)
{
    while (client->pending_write_remaining > 0) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(client->fd, client->pending_write_iov, (int) client->pending_write_iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                client->last_send_wouldblock = true;
                client->send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            clear_pending_write(client);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            client->last_send_wouldblock = true;
            client->send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }

        client->write_syscalls++;
        client->bytes_sent += (size_t) written;
        consume_pending_write(client, (size_t) written);
    }

    client->request_offset += client->pending_write_payload_len;
    clear_pending_write(client);
    return 0;
}

static int write_data_frame_nonblocking(poc_client *client, const uint8_t *framehd, size_t length)
{
    if (client->pending_write_remaining == 0 && prepare_pending_data_frame_write(client, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return flush_pending_data_frame_write(client);
}

static int send_data_callback(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    bool new_data_frame = client->pending_write_remaining == 0;
    (void) session;
    (void) frame;
    (void) source;

    client->send_data_callback_calls++;
    if (new_data_frame) {
        client->data_frames_sent++;
        client->data_bytes_sent += length;
        if (length > client->max_data_frame_len) {
            client->max_data_frame_len = length;
        }
        if (client->min_data_frame_len == 0 || length < client->min_data_frame_len) {
            client->min_data_frame_len = length;
        }
    }

    if (client->poll_loop) {
        int write_rv = write_data_frame_nonblocking(client, framehd, length);
        if (write_rv == NGHTTP2_ERR_WOULDBLOCK) {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        if (write_rv != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    } else if (write_data_frame(client, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    record_data_sent(client);
    client->pending_data_len = 0;

    return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) flags;
    if (stream_id == client->stream_id && len > 0) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->data_recv_calls++;
        client->call_data_recv_calls++;
        client->response_data_bytes += len;
        client->call_response_data_bytes += len;
        if (elapsed > 0) {
            if (client->first_response_data_us == 0) {
                client->first_response_data_us = elapsed;
            }
            client->last_response_data_us = elapsed;
        }
        if (client->awaiting_data_after_poll && client->last_poll_return_abs_us > 0) {
            uint64_t delta = monotonic_us() - client->last_poll_return_abs_us;
            client->call_poll_to_data_us += delta;
            if (delta > client->call_max_poll_to_data_us) {
                client->call_max_poll_to_data_us = delta;
            }
            client->awaiting_data_after_poll = false;
        }
        if (client->awaiting_data_after_window_update_sent && client->last_window_update_sent_abs_us > 0) {
            uint64_t delta = monotonic_us() - client->last_window_update_sent_abs_us;
            client->call_window_update_to_data_us += delta;
            if (delta > client->call_max_window_update_to_data_us) {
                client->call_max_window_update_to_data_us = delta;
            }
            client->awaiting_data_after_window_update_sent = false;
        }
        if (client->direct_response_payload && client->decode_response_incrementally && ((client->response_fci != NULL && client->response_fcc != NULL) || client->queue_response_payloads)) {
            if (process_response_data_direct(client, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        } else if (!client->discard_response_body) {
            uint64_t append_started = monotonic_us();
            uint64_t append_elapsed;
            smart_str_appendl(&client->body, (const char *) data, len);
            append_elapsed = monotonic_us() - append_started;
            if (client->body.s != NULL && ZSTR_LEN(client->body.s) > client->call_max_body_buffer_bytes) {
                client->call_max_body_buffer_bytes = ZSTR_LEN(client->body.s);
            }
            client->call_body_append_us += append_elapsed;
            if (append_elapsed > client->call_max_body_append_us) {
                client->call_max_body_append_us = append_elapsed;
            }
            if (client->decode_response_incrementally && client->response_fci != NULL && client->response_fcc != NULL) {
                zend_long decoded_messages = 0;
                uint64_t payload_string_us = 0;
                uint64_t max_payload_string_us = 0;
                uint64_t decode_us = 0;
                uint64_t max_decode_us = 0;
                if (process_response_messages_from_offset(client, client->response_fci, client->response_fcc, &client->response_parse_offset, false, &decoded_messages, &payload_string_us, &max_payload_string_us, &decode_us, &max_decode_us) != 0) {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
                client->call_decoded_messages += decoded_messages;
                client->call_response_payload_string_us += payload_string_us;
                if (max_payload_string_us > client->call_max_response_payload_string_us) {
                    client->call_max_response_payload_string_us = max_payload_string_us;
                }
                client->call_response_decode_us += decode_us;
                if (max_decode_us > client->call_max_response_decode_us) {
                    client->call_max_response_decode_us = max_decode_us;
                }
                compact_response_body_if_needed(client);
            }
        }
    }
    return 0;
}

static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) flags;
    if (frame->hd.stream_id != client->stream_id) {
        return 0;
    }
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->grpc_status = atoi(status_buf);
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        if (client->first_response_header_us == 0) {
            client->first_response_header_us = monotonic_us() - client->call_started_us;
        }
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->http_status = atoi(status_buf);
    } else if (namelen == sizeof("x-bench-server-handler-ns") - 1 && memcmp(name, "x-bench-server-handler-ns", namelen) == 0) {
        client->server_handler_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-alloc-ns") - 1 && memcmp(name, "x-bench-server-payload-alloc-ns", namelen) == 0) {
        client->server_payload_alloc_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-bytes") - 1 && memcmp(name, "x-bench-server-payload-bytes", namelen) == 0) {
        client->server_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-request-payload-bytes") - 1 && memcmp(name, "x-bench-server-request-payload-bytes", namelen) == 0) {
        client->server_request_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-start-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-start-ns", namelen) == 0) {
        client->server_stats_handler_start_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-end-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-end-ns", namelen) == 0) {
        client->server_stats_handler_end_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-in-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-in-payload-ns", namelen) == 0) {
        client->server_stats_in_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-header-ns") - 1 && memcmp(name, "x-bench-server-stats-out-header-ns", namelen) == 0) {
        client->server_stats_out_header_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-out-payload-ns", namelen) == 0) {
        client->server_stats_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-first-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-first-out-payload-ns", namelen) == 0) {
        client->server_stats_first_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-last-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-last-out-payload-ns", namelen) == 0) {
        client->server_stats_last_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-count") - 1 && memcmp(name, "x-bench-server-stats-out-payload-count", namelen) == 0) {
        client->server_stats_out_payload_count = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-bytes", namelen) == 0) {
        client->server_stats_out_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-wire-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-wire-bytes", namelen) == 0) {
        client->server_stats_out_payload_wire_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-compressed-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-compressed-bytes", namelen) == 0) {
        client->server_stats_out_payload_compressed_bytes = header_value_to_long(value, valuelen);
    }
    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) error_code;
    if (stream_id == client->stream_id) {
        client->stream_closed = true;
        client->stream_error_code = error_code;
        client->stream_closed_us = monotonic_us() - client->call_started_us;
    }
    return 0;
}

static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->sent_frames++;
    client->last_sent_frame_type = frame->hd.type;
    client->last_sent_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_DATA && !client->no_copy) {
        client->data_frames_sent++;
        client->data_bytes_sent += frame->hd.length;
        record_data_sent(client);
        if (frame->hd.length > client->max_data_frame_len) {
            client->max_data_frame_len = frame->hd.length;
        }
        if (client->min_data_frame_len == 0 || frame->hd.length < client->min_data_frame_len) {
            client->min_data_frame_len = frame->hd.length;
        }
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->window_update_frames_sent++;
        client->call_window_update_frames_sent++;
        if (frame->hd.stream_id == 0) {
            client->connection_window_update_frames_sent++;
            client->call_connection_window_update_frames_sent++;
            client->connection_window_update_increment_sent += frame->window_update.window_size_increment;
            client->call_connection_window_update_increment_sent += frame->window_update.window_size_increment;
        } else {
            client->stream_window_update_frames_sent++;
            client->call_stream_window_update_frames_sent++;
            client->stream_window_update_increment_sent += frame->window_update.window_size_increment;
            client->call_stream_window_update_increment_sent += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (client->first_window_update_sent_us == 0) {
                client->first_window_update_sent_us = elapsed;
            }
            client->last_window_update_sent_us = elapsed;
            client->last_window_update_sent_abs_us = monotonic_us();
            client->awaiting_data_after_window_update_sent = true;
        }
    }
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->recv_frames++;
    client->last_recv_frame_type = frame->hd.type;
    client->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        mark_channel_draining(client->channel, frame->goaway.last_stream_id, frame->goaway.error_code);
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->window_update_frames_recv++;
        client->call_window_update_frames_recv++;
        if (frame->hd.stream_id == 0) {
            client->connection_window_update_frames_recv++;
            client->call_connection_window_update_frames_recv++;
            client->connection_window_update_increment_recv += frame->window_update.window_size_increment;
            client->call_connection_window_update_increment_recv += frame->window_update.window_size_increment;
        } else {
            client->stream_window_update_frames_recv++;
            client->call_stream_window_update_frames_recv++;
            client->stream_window_update_increment_recv += frame->window_update.window_size_increment;
            client->call_stream_window_update_increment_recv += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (client->first_window_update_us == 0) {
                client->first_window_update_us = elapsed;
            }
            client->last_window_update_us = elapsed;
        }
    }
    return 0;
}

static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
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

static zend_long header_value_to_long(const uint8_t *value, size_t valuelen)
{
    char buf[32];
    size_t copy_len = valuelen < sizeof(buf) - 1 ? valuelen : sizeof(buf) - 1;
    memcpy(buf, value, copy_len);
    buf[copy_len] = '\0';
    return (zend_long) atoll(buf);
}

static int process_response_messages(poc_client *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long *decoded_messages, uint64_t *decode_us, uint64_t *max_decode_us)
{
    size_t offset = 0;
    uint64_t payload_string_us = 0;
    uint64_t max_payload_string_us = 0;
    return process_response_messages_from_offset(client, fci, fcc, &offset, true, decoded_messages, &payload_string_us, &max_payload_string_us, decode_us, max_decode_us);
}

static int process_response_messages_from_offset(poc_client *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us)
{
    zend_string *body;
    const char *data;
    size_t len;

    *decoded_messages = 0;
    *payload_string_us = 0;
    *max_payload_string_us = 0;
    *decode_us = 0;
    *max_decode_us = 0;

    if (client->body.s == NULL) {
        return 0;
    }

    smart_str_0(&client->body);
    body = client->body.s;
    data = ZSTR_VAL(body);
    len = ZSTR_LEN(body);

    while (*offset < len) {
        uint32_t payload_len;
        zval params[1];
        zval retval;
        uint64_t payload_string_started;
        uint64_t payload_string_elapsed;
        uint64_t started;
        uint64_t elapsed;

        if (len - *offset < 5) {
            return require_complete ? -1 : 0;
        }
        payload_len = ((uint32_t) (unsigned char) data[*offset + 1] << 24)
            | ((uint32_t) (unsigned char) data[*offset + 2] << 16)
            | ((uint32_t) (unsigned char) data[*offset + 3] << 8)
            | (uint32_t) (unsigned char) data[*offset + 4];
        if (payload_len > len - *offset - 5) {
            return require_complete ? -1 : 0;
        }
        *offset += 5;

        if (client->call_started_us != 0) {
            uint64_t ready_us = monotonic_us() - client->call_started_us;
            if (client->first_response_message_ready_us == 0) {
                client->first_response_message_ready_us = ready_us;
            }
            client->last_response_message_ready_us = ready_us;
        }
        payload_string_started = monotonic_us();
        ZVAL_STRINGL(&params[0], data + *offset, payload_len);
        payload_string_elapsed = monotonic_us() - payload_string_started;
        *payload_string_us += payload_string_elapsed;
        if (payload_string_elapsed > *max_payload_string_us) {
            *max_payload_string_us = payload_string_elapsed;
        }
        ZVAL_UNDEF(&retval);
        fci->params = params;
        fci->param_count = 1;
        fci->retval = &retval;

        started = monotonic_us();
        if (zend_call_function(fci, fcc) != SUCCESS) {
            zval_ptr_dtor(&params[0]);
            if (!Z_ISUNDEF(retval)) {
                zval_ptr_dtor(&retval);
            }
            return -1;
        }
        elapsed = monotonic_us() - started;
        if (client->call_started_us != 0) {
            uint64_t done_us = monotonic_us() - client->call_started_us;
            if (client->first_response_callback_done_us == 0) {
                client->first_response_callback_done_us = done_us;
            }
            client->last_response_callback_done_us = done_us;
        }
        *decode_us += elapsed;
        if (elapsed > *max_decode_us) {
            *max_decode_us = elapsed;
        }
        (*decoded_messages)++;

        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        *offset += payload_len;
    }

    return 0;
}

static int process_response_data_direct(poc_client *client, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
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
            client->response_payload_offset = 0;
            if (client->transport_thread) {
                client->response_raw_payload = client->response_payload_len > 0 ? malloc(client->response_payload_len) : malloc(1);
                if (client->response_raw_payload == NULL) {
                    return -1;
                }
            } else {
                client->response_payload = zend_string_alloc(client->response_payload_len, 0);
                if (client->response_payload_len == 0) {
                ZSTR_VAL(client->response_payload)[0] = '\0';
                }
            }
        }

        if (client->transport_thread && client->response_raw_payload != NULL) {
            size_t need = client->response_payload_len - client->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            uint64_t payload_string_started = monotonic_us();
            uint64_t payload_string_elapsed;
            if (take > 0) {
                memcpy(client->response_raw_payload + client->response_payload_offset, data + offset, take);
                client->response_payload_offset += take;
                offset += take;
            }
            payload_string_elapsed = monotonic_us() - payload_string_started;
            client->call_response_payload_string_us += payload_string_elapsed;
            if (payload_string_elapsed > client->call_max_response_payload_string_us) {
                client->call_max_response_payload_string_us = payload_string_elapsed;
            }
            if (client->response_payload_offset == client->response_payload_len) {
                char *payload = client->response_raw_payload;
                size_t payload_len = client->response_payload_len;
                uint64_t ready_abs_us = monotonic_us();

                if (client->call_started_us != 0) {
                    uint64_t ready_us = ready_abs_us - client->call_started_us;
                    if (client->first_response_message_ready_us == 0) {
                        client->first_response_message_ready_us = ready_us;
                    }
                    client->last_response_message_ready_us = ready_us;
                }
                client->response_raw_payload = NULL;
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;
                if (enqueue_raw_response_payload(client, payload, payload_len, ready_abs_us) != 0) {
                    free(payload);
                    return -1;
                }
            }
        } else if (client->response_payload != NULL) {
            size_t need = client->response_payload_len - client->response_payload_offset;
            size_t take = need < len - offset ? need : len - offset;
            uint64_t payload_string_started = monotonic_us();
            uint64_t payload_string_elapsed;
            if (take > 0) {
                memcpy(ZSTR_VAL(client->response_payload) + client->response_payload_offset, data + offset, take);
                client->response_payload_offset += take;
                offset += take;
            }
            payload_string_elapsed = monotonic_us() - payload_string_started;
            client->call_response_payload_string_us += payload_string_elapsed;
            if (payload_string_elapsed > client->call_max_response_payload_string_us) {
                client->call_max_response_payload_string_us = payload_string_elapsed;
            }

            if (client->response_payload_offset == client->response_payload_len) {
                zend_string *payload = client->response_payload;
                uint64_t ready_abs_us = monotonic_us();

                if (client->call_started_us != 0) {
                    uint64_t ready_us = ready_abs_us - client->call_started_us;
                    if (client->first_response_message_ready_us == 0) {
                        client->first_response_message_ready_us = ready_us;
                    }
                    client->last_response_message_ready_us = ready_us;
                }
                ZSTR_VAL(payload)[client->response_payload_len] = '\0';
                client->response_payload = NULL;
                client->response_header_len = 0;
                client->response_payload_len = 0;
                client->response_payload_offset = 0;

                if (client->queue_response_payloads || client->read_ahead_delivery) {
                    if (enqueue_response_payload(client, payload) != 0) {
                        zend_string_release(payload);
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

static int enqueue_response_payload(poc_client *client, zend_string *payload)
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
    if (client->response_queue_count > client->call_max_response_queue_count) {
        client->call_max_response_queue_count = client->response_queue_count;
    }
    if (client->response_queue_bytes > client->call_max_response_queue_bytes) {
        client->call_max_response_queue_bytes = client->response_queue_bytes;
    }

    if (deliver_queued_response_payloads_if_bounded(client) != 0) {
        return -1;
    }

    return 0;
}

static int deliver_response_payload(poc_client *client, zend_string *payload, uint64_t ready_abs_us)
{
    zval params[1];
    zval retval;
    uint64_t started;
    uint64_t elapsed;
    uint64_t now = monotonic_us();
    uint64_t queue_wait = ready_abs_us > 0 && now >= ready_abs_us ? now - ready_abs_us : 0;

    client->call_response_queue_wait_us += queue_wait;
    if (queue_wait > client->call_max_response_queue_wait_us) {
        client->call_max_response_queue_wait_us = queue_wait;
    }

    ZVAL_STR(&params[0], payload);
    ZVAL_UNDEF(&retval);
    client->response_fci->params = params;
    client->response_fci->param_count = 1;
    client->response_fci->retval = &retval;

    started = monotonic_us();
    if (zend_call_function(client->response_fci, client->response_fcc) != SUCCESS) {
        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        return -1;
    }
    elapsed = monotonic_us() - started;
    if (client->call_started_us != 0) {
        uint64_t done_us = monotonic_us() - client->call_started_us;
        if (client->first_response_callback_done_us == 0) {
            client->first_response_callback_done_us = done_us;
        }
        client->last_response_callback_done_us = done_us;
    }
    client->call_response_decode_us += elapsed;
    if (elapsed > client->call_max_response_decode_us) {
        client->call_max_response_decode_us = elapsed;
    }
    client->call_decoded_messages++;

    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
    }

    return 0;
}

static int deliver_queued_response_payloads(poc_client *client)
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

static int deliver_queued_response_payloads_if_bounded(poc_client *client)
{
    bool over_message_limit = client->read_ahead_max_messages > 0 && client->response_queue_count >= client->read_ahead_max_messages;
    bool over_byte_limit = client->read_ahead_max_bytes > 0 && client->response_queue_bytes >= client->read_ahead_max_bytes;

    if (!over_message_limit && !over_byte_limit) {
        return 0;
    }

    return deliver_queued_response_payloads(client);
}

static void free_queued_response_payloads(poc_client *client)
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

static int enqueue_raw_response_payload(poc_client *client, char *payload, size_t len, uint64_t ready_abs_us)
{
    queued_raw_payload *entry = malloc(sizeof(queued_raw_payload));
    if (entry == NULL) {
        return -1;
    }
    entry->payload = payload;
    entry->len = len;
    entry->ready_abs_us = ready_abs_us;
    entry->next = NULL;

    if (client->raw_response_queue_tail == NULL) {
        client->raw_response_queue_head = entry;
    } else {
        client->raw_response_queue_tail->next = entry;
    }
    client->raw_response_queue_tail = entry;
    client->response_queue_count++;
    client->response_queue_bytes += len;
    if (client->response_queue_count > client->call_max_response_queue_count) {
        client->call_max_response_queue_count = client->response_queue_count;
    }
    if (client->response_queue_bytes > client->call_max_response_queue_bytes) {
        client->call_max_response_queue_bytes = client->response_queue_bytes;
    }

    return 0;
}

static int deliver_raw_response_payload(poc_client *client, char *payload, size_t len, uint64_t ready_abs_us)
{
    zval params[1];
    zval retval;
    uint64_t started;
    uint64_t elapsed;
    uint64_t now = monotonic_us();
    uint64_t queue_wait = ready_abs_us > 0 && now >= ready_abs_us ? now - ready_abs_us : 0;

    client->call_response_queue_wait_us += queue_wait;
    if (queue_wait > client->call_max_response_queue_wait_us) {
        client->call_max_response_queue_wait_us = queue_wait;
    }

    ZVAL_STRINGL(&params[0], payload, len);
    ZVAL_UNDEF(&retval);
    client->response_fci->params = params;
    client->response_fci->param_count = 1;
    client->response_fci->retval = &retval;

    started = monotonic_us();
    if (zend_call_function(client->response_fci, client->response_fcc) != SUCCESS) {
        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        return -1;
    }
    elapsed = monotonic_us() - started;
    if (client->call_started_us != 0) {
        uint64_t done_us = monotonic_us() - client->call_started_us;
        if (client->first_response_callback_done_us == 0) {
            client->first_response_callback_done_us = done_us;
        }
        client->last_response_callback_done_us = done_us;
    }
    client->call_response_decode_us += elapsed;
    if (elapsed > client->call_max_response_decode_us) {
        client->call_max_response_decode_us = elapsed;
    }
    client->call_decoded_messages++;

    zval_ptr_dtor(&params[0]);
    if (!Z_ISUNDEF(retval)) {
        zval_ptr_dtor(&retval);
    }

    return 0;
}

static int deliver_queued_raw_response_payloads(poc_client *client)
{
    while (client->raw_response_queue_head != NULL) {
        queued_raw_payload *entry = client->raw_response_queue_head;
        client->raw_response_queue_head = entry->next;
        if (client->raw_response_queue_head == NULL) {
            client->raw_response_queue_tail = NULL;
        }
        client->response_queue_count--;
        client->response_queue_bytes -= entry->len;
        if (deliver_raw_response_payload(client, entry->payload, entry->len, entry->ready_abs_us) != 0) {
            free(entry->payload);
            free(entry);
            return -1;
        }
        free(entry->payload);
        free(entry);
    }

    return 0;
}

static void free_queued_raw_response_payloads(poc_client *client)
{
    while (client->raw_response_queue_head != NULL) {
        queued_raw_payload *entry = client->raw_response_queue_head;
        client->raw_response_queue_head = entry->next;
        free(entry->payload);
        free(entry);
    }
    client->raw_response_queue_tail = NULL;
    client->response_queue_count = 0;
    client->response_queue_bytes = 0;
}

static void compact_response_body_if_needed(poc_client *client)
{
    zend_string *body;
    size_t len;
    size_t consumed;
    size_t remaining;
    uint64_t started;
    uint64_t elapsed;

    if (!client->compact_response_buffer || client->body.s == NULL || client->response_parse_offset == 0) {
        return;
    }
    if (client->response_parse_offset < client->response_compact_threshold) {
        return;
    }

    body = client->body.s;
    len = ZSTR_LEN(body);
    consumed = client->response_parse_offset;
    if (consumed > len) {
        consumed = len;
    }
    remaining = len - consumed;

    started = monotonic_us();
    if (remaining > 0) {
        memmove(ZSTR_VAL(body), ZSTR_VAL(body) + consumed, remaining);
    }
    ZSTR_LEN(body) = remaining;
    ZSTR_VAL(body)[remaining] = '\0';
    elapsed = monotonic_us() - started;

    client->response_parse_offset = 0;
    client->call_body_compact_count++;
    client->call_body_compact_bytes += consumed;
    client->call_body_compact_us += elapsed;
    if (elapsed > client->call_max_body_compact_us) {
        client->call_max_body_compact_us = elapsed;
    }
}

static void record_data_sent(poc_client *client)
{
    uint64_t elapsed = monotonic_us() - client->call_started_us;
    if (client->first_data_sent_us == 0) {
        client->first_data_sent_us = elapsed;
    }
    client->last_data_sent_us = elapsed;
}

static int receive_available(nghttp2_session *session, poc_client *client, char *recv_buf, size_t recv_buf_len)
{
    size_t reads = 0;
    size_t bytes = 0;
    client->call_receive_drains++;
    for (;;) {
        uint64_t recv_started = monotonic_us();
        ssize_t nread = recv(client->fd, recv_buf, recv_buf_len, 0);
        uint64_t recv_elapsed = monotonic_us() - recv_started;
        if (nread > 0) {
            int rv;
            uint64_t mem_recv_started;
            uint64_t mem_recv_elapsed;
            client->call_recv_syscalls++;
            client->call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > client->call_max_recv_syscall_us) {
                client->call_max_recv_syscall_us = recv_elapsed;
            }
            reads++;
            bytes += (size_t) nread;
            client->bytes_received += (size_t) nread;
            mem_recv_started = monotonic_us();
            rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
            mem_recv_elapsed = monotonic_us() - mem_recv_started;
            client->call_mem_recv_us += mem_recv_elapsed;
            if (mem_recv_elapsed > client->call_max_mem_recv_us) {
                client->call_max_mem_recv_us = mem_recv_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
            if (client->flush_after_mem_recv && nghttp2_session_want_write(session)) {
                uint64_t send_started = monotonic_us();
                uint64_t send_elapsed;
                rv = nghttp2_session_send(session);
                send_elapsed = monotonic_us() - send_started;
                client->call_session_send_after_recv_us += send_elapsed;
                if (send_elapsed > client->call_max_session_send_after_recv_us) {
                    client->call_max_session_send_after_recv_us = send_elapsed;
                }
                if (rv < 0) {
                    return rv;
                }
            }
            continue;
        }
        if (nread == 0) {
            return NGHTTP2_ERR_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            client->call_recv_syscalls++;
            client->call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > client->call_max_recv_syscall_us) {
                client->call_max_recv_syscall_us = recv_elapsed;
            }
            client->recv_wouldblock_calls++;
            if (reads > 0) {
                client->call_receive_drains_with_data++;
                client->call_receive_drains_eagain_after_data++;
                if (reads > client->call_max_reads_per_drain) {
                    client->call_max_reads_per_drain = reads;
                }
                if (bytes > client->call_max_bytes_per_drain) {
                    client->call_max_bytes_per_drain = bytes;
                }
            }
            return 0;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
}

static int drive_stream_poll(nghttp2_session *session, poc_client *client, char *recv_buf, size_t recv_buf_len)
{
    while (!client->stream_closed && (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))) {
        int rv;

        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        if (client->read_first_poll_loop && nghttp2_session_want_read(session)) {
            rv = receive_available(session, client, recv_buf, recv_buf_len);
            if (rv < 0) {
                return rv;
            }
            if (client->read_ahead_delivery && deliver_queued_response_payloads(client) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (client->stream_closed) {
                return 0;
            }
        }

        do {
            uint64_t send_started;
            uint64_t send_elapsed;
            client->last_send_wouldblock = false;
            send_started = monotonic_us();
            rv = nghttp2_session_send(session);
            send_elapsed = monotonic_us() - send_started;
            client->call_session_send_after_recv_us += send_elapsed;
            if (send_elapsed > client->call_max_session_send_after_recv_us) {
                client->call_max_session_send_after_recv_us = send_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
        } while (!client->last_send_wouldblock && nghttp2_session_want_write(session));

        rv = receive_available(session, client, recv_buf, recv_buf_len);
        if (rv < 0) {
            return rv;
        }
        if (client->read_ahead_delivery && deliver_queued_response_payloads(client) != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (client->stream_closed) {
            return 0;
        }

        short events = 0;
        if (nghttp2_session_want_read(session)) {
            events |= POLLIN;
        }
        if (nghttp2_session_want_write(session)) {
            events |= POLLOUT;
        }
        if (events == 0) {
            break;
        }

        struct pollfd pfd;
        pfd.fd = client->fd;
        pfd.events = events;
        pfd.revents = 0;
        client->poll_calls++;
        uint64_t poll_started = monotonic_us();
        int poll_timeout_ms = 5000;
        if (client->deadline_abs_us > 0) {
            uint64_t remaining_us;
            if (poll_started >= client->deadline_abs_us) {
                client->timed_out = true;
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            remaining_us = client->deadline_abs_us - poll_started;
            poll_timeout_ms = (int) ((remaining_us + 999) / 1000);
            if (poll_timeout_ms < 1) {
                poll_timeout_ms = 1;
            } else if (poll_timeout_ms > 5000) {
                poll_timeout_ms = 5000;
            }
        }
        rv = poll(&pfd, 1, poll_timeout_ms);
        uint64_t poll_elapsed = monotonic_us() - poll_started;
        client->call_poll_wait_us += poll_elapsed;
        if (poll_elapsed > client->call_max_poll_wait_us) {
            client->call_max_poll_wait_us = poll_elapsed;
        }
        if (rv == 0) {
            client->poll_timeouts++;
            if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
                client->timed_out = true;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->poll_errors++;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if ((pfd.revents & POLLIN) != 0) {
            client->call_pollin_ready++;
            client->last_poll_return_abs_us = monotonic_us();
            client->awaiting_data_after_poll = true;
        }
        if ((pfd.revents & POLLOUT) != 0) {
            client->call_pollout_ready++;
        }
    }

    return client->stream_closed ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
}

static void *transport_thread_main(void *arg)
{
    transport_thread_args *args = (transport_thread_args *) arg;
    char *recv_buf = malloc(args->recv_buf_len);
    if (recv_buf == NULL) {
        args->rv = NGHTTP2_ERR_CALLBACK_FAILURE;
        return NULL;
    }

    args->rv = drive_stream_poll(args->session, args->client, recv_buf, args->recv_buf_len);
    free(recv_buf);

    return NULL;
}

static ssize_t mux_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    mux_context *ctx = (mux_context *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;
    while (total_written < length) {
        ssize_t written = send(ctx->fd, data + total_written, length - total_written, 0);
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        total_written += (size_t) written;
    }
    ctx->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static ssize_t mux_data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    mux_stream *stream = (mux_stream *) source->ptr;
    size_t remaining;
    size_t to_copy;
    (void) session;
    (void) stream_id;
    (void) user_data;

    *data_flags = 0;
    if (stream->request_offset >= stream->request_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    remaining = stream->request_len - stream->request_offset;
    to_copy = remaining < length ? remaining : length;
    memcpy(buf, stream->request + stream->request_offset, to_copy);
    stream->request_offset += to_copy;
    if (stream->request_offset >= stream->request_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) to_copy;
}

static int mux_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    mux_stream *stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    (void) flags;
    (void) user_data;
    if (stream != NULL && len > 0) {
        smart_str_appendl(&stream->body, (const char *) data, len);
    }
    return 0;
}

static int mux_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    mux_stream *stream;
    (void) flags;
    (void) user_data;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_HEADERS) {
        return 0;
    }
    stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (stream == NULL) {
        return 0;
    }
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        stream->grpc_status = (int) header_value_to_long(value, valuelen);
    }
    return 0;
}

static int mux_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    mux_context *ctx = (mux_context *) user_data;
    mux_stream *stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    (void) session;
    if (stream != NULL && !stream->stream_closed) {
        stream->stream_closed = true;
        stream->stream_error_code = error_code;
        ctx->closed_count++;
    }
    return 0;
}

PHP_FUNCTION(nghttp2_poc_multiplex_unary)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zend_long stream_count = 0;
    mux_context ctx;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_nv nva[7];
    size_t nvlen = 0;
    char authority[512];
    char recv_buf[16384];
    uint64_t started;
    int rv = 0;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_LONG(stream_count)
    ZEND_PARSE_PARAMETERS_END();

    if (stream_count <= 0 || stream_count > 256) {
        zend_throw_exception(NULL, "stream_count must be between 1 and 256", 0);
        RETURN_THROWS();
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = connect_tcp(host, port);
    if (ctx.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }
    ctx.stream_count = (size_t) stream_count;
    ctx.streams = ecalloc(ctx.stream_count, sizeof(mux_stream));

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, mux_send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, mux_on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, mux_on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, mux_on_stream_close_callback);
    nghttp2_session_client_new(&session, callbacks, &ctx);
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    for (size_t i = 0; i < ctx.stream_count; i++) {
        nghttp2_data_provider data_provider;
        memset(&data_provider, 0, sizeof(data_provider));
        ctx.streams[i].grpc_status = -1;
        ctx.streams[i].request = (const uint8_t *) request;
        ctx.streams[i].request_len = request_len;
        data_provider.source.ptr = &ctx.streams[i];
        data_provider.read_callback = mux_data_source_read_callback;
        ctx.streams[i].stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, &ctx.streams[i]);
        if (ctx.streams[i].stream_id < 0) {
            rv = -1;
            break;
        }
    }

    started = monotonic_us();
    if (rv == 0 && nghttp2_session_send(session) != 0) {
        rv = -1;
    }
    while (rv == 0 && ctx.closed_count < ctx.stream_count) {
        ssize_t nread = recv(ctx.fd, recv_buf, sizeof(recv_buf), 0);
        if (nread <= 0) {
            rv = -1;
            break;
        }
        ctx.bytes_received += (size_t) nread;
        if (nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread) < 0) {
            rv = -1;
            break;
        }
        if (nghttp2_session_send(session) != 0) {
            rv = -1;
            break;
        }
    }

    array_init(return_value);
    add_assoc_long(return_value, "streams", (zend_long) ctx.stream_count);
    add_assoc_long(return_value, "closed", (zend_long) ctx.closed_count);
    add_assoc_long(return_value, "elapsed_us", (zend_long) (monotonic_us() - started));
    add_assoc_long(return_value, "bytes_sent", (zend_long) ctx.bytes_sent);
    add_assoc_long(return_value, "bytes_received", (zend_long) ctx.bytes_received);
    add_assoc_bool(return_value, "ok", rv == 0 && ctx.closed_count == ctx.stream_count);
    zval statuses;
    array_init(&statuses);
    for (size_t i = 0; i < ctx.stream_count; i++) {
        add_next_index_long(&statuses, ctx.streams[i].grpc_status);
        smart_str_free(&ctx.streams[i].body);
    }
    add_assoc_zval(return_value, "grpc_statuses", &statuses);

    close(ctx.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    efree(ctx.streams);
}

PHP_FUNCTION(nghttp2_poc_channel_open)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    bool use_tls = false;
    char *root_certs = NULL;
    size_t root_certs_len = 0;
    char *cert_chain = NULL;
    size_t cert_chain_len = 0;
    char *private_key = NULL;
    size_t private_key_len = 0;
    poc_channel *channel;
    const char *error_message = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 6)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(use_tls)
        Z_PARAM_STRING_OR_NULL(root_certs, root_certs_len)
        Z_PARAM_STRING_OR_NULL(cert_chain, cert_chain_len)
        Z_PARAM_STRING_OR_NULL(private_key, private_key_len)
    ZEND_PARSE_PARAMETERS_END();

    channel = create_poc_channel(host, port, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, false, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open channel", 0);
        RETURN_THROWS();
    }
    RETURN_RES(zend_register_resource(channel, le_poc_channel));
}

PHP_FUNCTION(nghttp2_poc_channel_is_usable)
{
    zval *channel_zv = NULL;
    poc_channel *channel;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(channel_zv)
    ZEND_PARSE_PARAMETERS_END();

    channel = (poc_channel *) zend_fetch_resource(Z_RES_P(channel_zv), "nghttp2_poc_channel", le_poc_channel);
    RETURN_BOOL(channel_usable(channel));
}

static int perform_poc_channel_unary(poc_channel *channel, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, bool channel_reused, bool persistent_reused, zval *return_value)
{
    poc_client client;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char header_storage[8][512];
    size_t header_storage_used = 0;
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
        zend_throw_exception(NULL, "invalid nghttp2_poc channel", 0);
        return FAILURE;
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "native channel already has an active stream", 0);
        return FAILURE;
    }

    memset(&client, 0, sizeof(client));
    client.fd = channel->fd;
    client.channel = channel;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    total_started = monotonic_us();
    client.deadline_abs_us = timeout_us > 0 ? total_started + (uint64_t) timeout_us : 0;
    if (set_socket_timeout_us(channel->fd, timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    setup_started = monotonic_us();
    nghttp2_session_set_user_data(channel->session, &client);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", channel->authority, strlen(channel->authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
            zend_string *value_str;
            if (key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    setup_us = monotonic_us() - setup_started;

    submit_started = monotonic_us();
    client.stream_id = nghttp2_submit_request(channel->session, NULL, nva, nvlen, &data_provider, NULL);
    submit_us = monotonic_us() - submit_started;
    if (client.stream_id < 0) {
        nghttp2_session_set_user_data(channel->session, NULL);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    initial_send_started = monotonic_us();
    rv = nghttp2_session_send(channel->session);
    initial_send_us = monotonic_us() - initial_send_started;
    if (rv != 0) {
        mark_channel_dead(channel, rv);
        nghttp2_session_set_user_data(channel->session, NULL);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        return FAILURE;
    }

    recv_loop_started = monotonic_us();
    while (!client.stream_closed) {
        ssize_t nread = channel_recv(channel, (uint8_t *) recv_buf, sizeof(recv_buf));
        if (nread <= 0) {
            if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && client.deadline_abs_us > 0 && monotonic_us() >= client.deadline_abs_us) {
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
            nghttp2_session_set_user_data(channel->session, NULL);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            return FAILURE;
        }
        rv = nghttp2_session_send(channel->session);
        if (rv != 0) {
            mark_channel_dead(channel, rv);
            nghttp2_session_set_user_data(channel->session, NULL);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
            return FAILURE;
        }
        client.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;
    nghttp2_session_set_user_data(channel->session, NULL);

    array_init(return_value);
    smart_str_0(&client.body);
    add_assoc_str(return_value, "body", client.body.s ? zend_string_copy(client.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
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
    add_assoc_long(return_value, "channel_last_goaway_error_code", channel->last_goaway_error_code);
    add_assoc_long(return_value, "channel_last_goaway_stream_id", channel->last_goaway_stream_id);
    add_assoc_long(return_value, "server_handler_ns", client.server_handler_ns);
    add_assoc_long(return_value, "server_payload_alloc_ns", client.server_payload_alloc_ns);
    add_assoc_long(return_value, "server_payload_bytes", client.server_payload_bytes);
    add_assoc_long(return_value, "server_request_payload_bytes", client.server_request_payload_bytes);
    add_assoc_long(return_value, "server_stats_handler_start_ns", client.server_stats_handler_start_ns);
    add_assoc_long(return_value, "server_stats_handler_end_ns", client.server_stats_handler_end_ns);
    add_assoc_long(return_value, "server_stats_in_payload_ns", client.server_stats_in_payload_ns);
    add_assoc_long(return_value, "server_stats_out_header_ns", client.server_stats_out_header_ns);
    add_assoc_long(return_value, "server_stats_out_payload_ns", client.server_stats_out_payload_ns);
    add_assoc_long(return_value, "server_stats_first_out_payload_ns", client.server_stats_first_out_payload_ns);
    add_assoc_long(return_value, "server_stats_last_out_payload_ns", client.server_stats_last_out_payload_ns);
    add_assoc_long(return_value, "server_stats_out_payload_count", client.server_stats_out_payload_count);
    add_assoc_long(return_value, "server_stats_out_payload_bytes", client.server_stats_out_payload_bytes);
    add_assoc_long(return_value, "server_stats_out_payload_wire_bytes", client.server_stats_out_payload_wire_bytes);
    add_assoc_long(return_value, "server_stats_out_payload_compressed_bytes", client.server_stats_out_payload_compressed_bytes);
    smart_str_free(&client.body);
    return SUCCESS;
}

PHP_FUNCTION(nghttp2_poc_channel_unary)
{
    zval *channel_zv = NULL;
    poc_channel *channel;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    zend_long timeout_us = 0;

    ZEND_PARSE_PARAMETERS_START(3, 5)
        Z_PARAM_RESOURCE(channel_zv)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_LONG(timeout_us)
    ZEND_PARSE_PARAMETERS_END();

    channel = (poc_channel *) zend_fetch_resource(Z_RES_P(channel_zv), "nghttp2_poc_channel", le_poc_channel);
    if (perform_poc_channel_unary(channel, path, path_len, request, request_len, headers_zv, timeout_us, true, false, return_value) != SUCCESS) {
        RETURN_THROWS();
    }
}

PHP_FUNCTION(nghttp2_poc_persistent_channel_unary)
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
    poc_channel *channel;
    bool persistent_reused = false;
    const char *error_message = NULL;

    ZEND_PARSE_PARAMETERS_START(5, 11)
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
    ZEND_PARSE_PARAMETERS_END();

    if (!NGHTTP2_POC_G(persistent_channels_initialized)) {
        zend_throw_exception(NULL, "persistent channel cache is not initialized", 0);
        RETURN_THROWS();
    }

    channel = zend_hash_str_find_ptr(&NGHTTP2_POC_G(persistent_channels), key, key_len);
    if (channel != NULL && !channel_usable(channel)) {
        destroy_poc_channel(channel);
        zend_hash_str_del(&NGHTTP2_POC_G(persistent_channels), key, key_len);
        channel = NULL;
    }

    if (channel == NULL) {
        channel = create_poc_channel(host, port, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, true, &error_message);
        if (channel == NULL) {
            zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
            RETURN_THROWS();
        }
        zend_hash_str_update_ptr(&NGHTTP2_POC_G(persistent_channels), key, key_len, channel);
    } else {
        persistent_reused = true;
    }

    if (perform_poc_channel_unary(channel, path, path_len, request, request_len, headers_zv, timeout_us, true, persistent_reused, return_value) != SUCCESS) {
        if (channel != NULL && !channel_usable(channel)) {
            destroy_poc_channel(channel);
            zend_hash_str_del(&NGHTTP2_POC_G(persistent_channels), key, key_len);
        }
        RETURN_THROWS();
    }

    if (!channel_usable(channel)) {
        destroy_poc_channel(channel);
        zend_hash_str_del(&NGHTTP2_POC_G(persistent_channels), key, key_len);
    }
}

PHP_FUNCTION(nghttp2_poc_stream_open)
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
    poc_channel *channel;
    poc_stream *stream;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char header_storage[8][512];
    size_t header_storage_used = 0;
    bool persistent_reused = false;
    const char *error_message = NULL;
    int rv;

    ZEND_PARSE_PARAMETERS_START(5, 11)
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
    ZEND_PARSE_PARAMETERS_END();

    channel = get_persistent_channel(key, key_len, host, port, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "native channel already has an active stream", 0);
        RETURN_THROWS();
    }
    if (set_socket_timeout_us(channel->fd, timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        discard_persistent_channel(key, key_len, channel);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        RETURN_THROWS();
    }

    stream = ecalloc(1, sizeof(poc_stream));
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
    stream->client.call_started_us = monotonic_us();
    stream->client.deadline_abs_us = timeout_us > 0 ? stream->client.call_started_us + (uint64_t) timeout_us : 0;
    stream->client.decode_response_incrementally = true;
    stream->client.direct_response_payload = true;
    stream->client.queue_response_payloads = true;
    set_grpc_header(&stream->client, stream->client.request_len);

    nghttp2_session_set_user_data(channel->session, &stream->client);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", channel->authority, strlen(channel->authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *header_key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), header_key, value) {
            zend_string *value_str;
            if (header_key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(header_key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(header_key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    stream->client.stream_id = nghttp2_submit_request(channel->session, NULL, nva, nvlen, &data_provider, NULL);
    if (stream->client.stream_id < 0) {
        destroy_poc_stream(stream);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        RETURN_THROWS();
    }

    channel->busy = true;
    rv = nghttp2_session_send(channel->session);
    if (rv != 0) {
        mark_channel_dead(channel, rv);
        channel->busy = false;
        destroy_poc_stream(stream);
        discard_persistent_channel(key, key_len, channel);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        RETURN_THROWS();
    }

    RETURN_RES(zend_register_resource(stream, le_poc_stream));
}

static void add_stream_status(zval *return_value, poc_stream *stream)
{
    poc_client *client = &stream->client;
    add_assoc_bool(return_value, "done", true);
    add_assoc_long(return_value, "grpc_status", client->grpc_status);
    add_assoc_long(return_value, "http_status", client->http_status);
    add_assoc_long(return_value, "stream_error_code", client->stream_error_code);
    add_assoc_bool(return_value, "timed_out", client->timed_out);
    add_assoc_bool(return_value, "cancelled", stream->cancelled);
    add_assoc_long(return_value, "body_bytes", 0);
    add_assoc_long(return_value, "bytes_sent", client->bytes_sent);
    add_assoc_long(return_value, "bytes_received", client->bytes_received);
    add_assoc_bool(return_value, "channel_dead", stream->channel != NULL ? stream->channel->dead : true);
    add_assoc_bool(return_value, "channel_draining", stream->channel != NULL ? stream->channel->draining : true);
    add_assoc_long(return_value, "server_handler_ns", client->server_handler_ns);
    add_assoc_long(return_value, "server_payload_alloc_ns", client->server_payload_alloc_ns);
    add_assoc_long(return_value, "server_payload_bytes", client->server_payload_bytes);
    add_assoc_long(return_value, "server_request_payload_bytes", client->server_request_payload_bytes);
    add_assoc_long(return_value, "server_stats_handler_start_ns", client->server_stats_handler_start_ns);
    add_assoc_long(return_value, "server_stats_handler_end_ns", client->server_stats_handler_end_ns);
    add_assoc_long(return_value, "server_stats_in_payload_ns", client->server_stats_in_payload_ns);
    add_assoc_long(return_value, "server_stats_out_header_ns", client->server_stats_out_header_ns);
    add_assoc_long(return_value, "server_stats_out_payload_ns", client->server_stats_out_payload_ns);
    add_assoc_long(return_value, "server_stats_first_out_payload_ns", client->server_stats_first_out_payload_ns);
    add_assoc_long(return_value, "server_stats_last_out_payload_ns", client->server_stats_last_out_payload_ns);
    add_assoc_long(return_value, "server_stats_out_payload_count", client->server_stats_out_payload_count);
    add_assoc_long(return_value, "server_stats_out_payload_bytes", client->server_stats_out_payload_bytes);
    add_assoc_long(return_value, "server_stats_out_payload_wire_bytes", client->server_stats_out_payload_wire_bytes);
    add_assoc_long(return_value, "server_stats_out_payload_compressed_bytes", client->server_stats_out_payload_compressed_bytes);
}

PHP_FUNCTION(nghttp2_poc_stream_next)
{
    zval *stream_zv = NULL;
    poc_stream *stream;
    poc_client *client;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (poc_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "nghttp2_poc_stream", le_poc_stream);
    if (stream == NULL) {
        RETURN_THROWS();
    }
    client = &stream->client;

    array_init(return_value);

    while (client->response_queue_head == NULL && !client->stream_closed && !stream->completed) {
        int rv;
        ssize_t nread;
        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            stream->completed = true;
            break;
        }
        rv = nghttp2_session_send(stream->channel->session);
        if (rv != 0) {
            mark_channel_dead(stream->channel, rv);
            stream->completed = true;
            break;
        }
        nread = channel_recv(stream->channel, (uint8_t *) stream->recv_buf, stream->recv_buf_len);
        if (nread <= 0) {
            if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
                client->timed_out = true;
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

    stream->completed = true;
    if (stream->channel != NULL) {
        nghttp2_session_set_user_data(stream->channel->session, NULL);
        stream->channel->busy = false;
    }
    add_stream_status(return_value, stream);
}

PHP_FUNCTION(nghttp2_poc_stream_cancel)
{
    zval *stream_zv = NULL;
    poc_stream *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (poc_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "nghttp2_poc_stream", le_poc_stream);
    if (stream != NULL && !stream->completed && stream->channel != NULL && channel_usable(stream->channel)) {
        stream->cancelled = true;
        stream->completed = true;
        stream->client.grpc_status = 1;
        nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        nghttp2_session_send(stream->channel->session);
        nghttp2_session_set_user_data(stream->channel->session, NULL);
        stream->channel->busy = false;
    }

    RETURN_TRUE;
}

PHP_FUNCTION(nghttp2_poc_unary)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    poc_client client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char authority[512];
    char header_storage[8][512];
    size_t header_storage_used = 0;
    int rv;
    char recv_buf[16384];
    uint64_t total_started = 0;
    uint64_t connect_started = 0;
    uint64_t connect_us = 0;
    uint64_t setup_started = 0;
    uint64_t setup_us = 0;
    uint64_t submit_started = 0;
    uint64_t submit_us = 0;
    uint64_t initial_send_started = 0;
    uint64_t initial_send_us = 0;
    uint64_t recv_loop_started = 0;
    uint64_t recv_loop_us = 0;
    uint64_t cleanup_started = 0;
    uint64_t cleanup_us = 0;

    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
    ZEND_PARSE_PARAMETERS_END();

    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    total_started = monotonic_us();

    connect_started = monotonic_us();
    client.fd = connect_tcp(host, port);
    connect_us = monotonic_us() - connect_started;
    if (client.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }

    setup_started = monotonic_us();
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);
    nghttp2_session_client_new(&session, callbacks, &client);
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
            zend_string *value_str;
            if (key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    setup_us = monotonic_us() - setup_started;

    submit_started = monotonic_us();
    client.stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, NULL);
    submit_us = monotonic_us() - submit_started;
    if (client.stream_id < 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        RETURN_THROWS();
    }

    initial_send_started = monotonic_us();
    rv = nghttp2_session_send(session);
    initial_send_us = monotonic_us() - initial_send_started;
    if (rv != 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        RETURN_THROWS();
    }

    recv_loop_started = monotonic_us();
    while (!client.stream_closed) {
        ssize_t nread = recv(client.fd, recv_buf, sizeof(recv_buf), 0);
        if (nread <= 0) {
            break;
        }
        client.bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
        if (rv < 0) {
            close(client.fd);
            nghttp2_session_del(session);
            nghttp2_session_callbacks_del(callbacks);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            RETURN_THROWS();
        }
        rv = nghttp2_session_send(session);
        if (rv != 0) {
            close(client.fd);
            nghttp2_session_del(session);
            nghttp2_session_callbacks_del(callbacks);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
            RETURN_THROWS();
        }
        client.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;

    cleanup_started = monotonic_us();
    close(client.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    cleanup_us = monotonic_us() - cleanup_started;

    array_init(return_value);
    smart_str_0(&client.body);
    add_assoc_str(return_value, "body", client.body.s ? zend_string_copy(client.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
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
    add_assoc_long(return_value, "total_us", (zend_long) (monotonic_us() - total_started));
    add_assoc_long(return_value, "connect_us", (zend_long) connect_us);
    add_assoc_long(return_value, "setup_us", (zend_long) setup_us);
    add_assoc_long(return_value, "submit_us", (zend_long) submit_us);
    add_assoc_long(return_value, "initial_send_us", (zend_long) initial_send_us);
    add_assoc_long(return_value, "recv_loop_us", (zend_long) recv_loop_us);
    add_assoc_long(return_value, "cleanup_us", (zend_long) cleanup_us);
    smart_str_free(&client.body);
}

PHP_FUNCTION(nghttp2_poc_unary_batch)
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
    bool transport_thread = false;
    zend_long timeout_us = 0;
    bool compact_response_buffer = false;
    zend_long response_compact_threshold = 0;
    zval *response_callback_zv = NULL;
    bool response_callback_enabled = false;
    zend_fcall_info response_fci;
    zend_fcall_info_cache response_fcc;
    poc_client client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char authority[512];
    char header_storage[8][512];
    size_t header_storage_used = 0;
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

    ZEND_PARSE_PARAMETERS_START(5, 26)
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
        Z_PARAM_BOOL(transport_thread)
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
    client.transport_thread = transport_thread && client.direct_response_payload;
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

    client.fd = connect_tcp(host, port);
    if (client.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
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
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
            zend_string *value_str;
            if (key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

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
        free_queued_raw_response_payloads(&client);
        if (client.response_raw_payload != NULL) {
            free(client.response_raw_payload);
            client.response_raw_payload = NULL;
        }
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
        smart_str_free(&client.body);
        memset(&client.body, 0, sizeof(client.body));

        client.stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, NULL);
        if (client.stream_id < 0) {
            failed++;
            break;
        }

        if (client.transport_thread) {
            pthread_t thread;
            transport_thread_args thread_args;
            thread_args.session = session;
            thread_args.client = &client;
            thread_args.recv_buf_len = recv_buf_len;
            thread_args.rv = 0;
            if (pthread_create(&thread, NULL, transport_thread_main, &thread_args) != 0) {
                failed++;
                break;
            }
            pthread_join(thread, NULL);
            rv = thread_args.rv;
            if (rv != 0) {
                failed++;
                break;
            }
            if (deliver_queued_raw_response_payloads(&client) != 0) {
                failed++;
                break;
            }
        } else if (poll_loop) {
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
        } else if (response_callback_enabled && decode_response_incrementally && client.direct_response_payload && (client.response_header_len != 0 || client.response_payload != NULL || client.response_raw_payload != NULL || client.response_queue_head != NULL || client.raw_response_queue_head != NULL)) {
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
    add_assoc_bool(return_value, "transport_thread", client.transport_thread);
    add_assoc_bool(return_value, "timed_out", client.timed_out);
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
    if (client.response_payload != NULL) {
        zend_string_release(client.response_payload);
        client.response_payload = NULL;
    }
    free_queued_response_payloads(&client);
    smart_str_free(&client.body);
}

PHP_GINIT_FUNCTION(nghttp2_poc)
{
#if defined(COMPILE_DL_NGHTTP2_POC) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_init(&nghttp2_poc_globals->persistent_channels, 8, NULL, NULL, 1);
    nghttp2_poc_globals->persistent_channels_initialized = true;
}

PHP_GSHUTDOWN_FUNCTION(nghttp2_poc)
{
    poc_channel *channel;

    if (!nghttp2_poc_globals->persistent_channels_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&nghttp2_poc_globals->persistent_channels, channel) {
        destroy_poc_channel(channel);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&nghttp2_poc_globals->persistent_channels);
    nghttp2_poc_globals->persistent_channels_initialized = false;
}

PHP_MINIT_FUNCTION(nghttp2_poc)
{
    le_poc_channel = zend_register_list_destructors_ex(poc_channel_dtor, NULL, "nghttp2_poc_channel", module_number);
    le_poc_stream = zend_register_list_destructors_ex(poc_stream_dtor, NULL, "nghttp2_poc_stream", module_number);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(nghttp2_poc)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "nghttp2_poc support", "enabled");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_unary, 0, 4, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_multiplex_unary, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, stream_count, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_nghttp2_poc_channel_open, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_tls, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_channel_is_usable, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_channel_unary, 0, 3, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, channel)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_persistent_channel_unary, 0, 5, IS_ARRAY, 0)
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
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_nghttp2_poc_stream_open, 0, 0, 5)
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
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_stream_next, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_stream_cancel, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_unary_batch, 0, 5, IS_ARRAY, 0)
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
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, transport_thread, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

static const zend_function_entry nghttp2_poc_functions[] = {
    PHP_FE(nghttp2_poc_multiplex_unary, arginfo_nghttp2_poc_multiplex_unary)
    PHP_FE(nghttp2_poc_channel_open, arginfo_nghttp2_poc_channel_open)
    PHP_FE(nghttp2_poc_channel_is_usable, arginfo_nghttp2_poc_channel_is_usable)
    PHP_FE(nghttp2_poc_channel_unary, arginfo_nghttp2_poc_channel_unary)
    PHP_FE(nghttp2_poc_persistent_channel_unary, arginfo_nghttp2_poc_persistent_channel_unary)
    PHP_FE(nghttp2_poc_stream_open, arginfo_nghttp2_poc_stream_open)
    PHP_FE(nghttp2_poc_stream_next, arginfo_nghttp2_poc_stream_next)
    PHP_FE(nghttp2_poc_stream_cancel, arginfo_nghttp2_poc_stream_cancel)
    PHP_FE(nghttp2_poc_unary, arginfo_nghttp2_poc_unary)
    PHP_FE(nghttp2_poc_unary_batch, arginfo_nghttp2_poc_unary_batch)
    PHP_FE_END
};

zend_module_entry nghttp2_poc_module_entry = {
    STANDARD_MODULE_HEADER,
    "nghttp2_poc",
    nghttp2_poc_functions,
    PHP_MINIT(nghttp2_poc),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(nghttp2_poc),
    "0.1.0",
    ZEND_MODULE_GLOBALS(nghttp2_poc),
    PHP_GINIT(nghttp2_poc),
    PHP_GSHUTDOWN(nghttp2_poc),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_NGHTTP2_POC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nghttp2_poc)
#endif
