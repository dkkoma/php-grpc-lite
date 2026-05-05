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
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAKE_NV(NAME, VALUE) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE}
#define MAKE_NV_L(NAME, VALUE, VALUE_LEN) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, (VALUE_LEN), NGHTTP2_NV_FLAG_NONE}
#define GRPC_NATIVE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES (64 * 1024 * 1024)
#define GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES 128
#define GRPC_LITE_MAX_RESPONSE_METADATA_BYTES (64 * 1024)

typedef struct _h2_channel h2_channel;
typedef struct _h2_stream h2_stream;

typedef struct queued_payload {
    zend_string *payload;
    uint64_t ready_abs_us;
    struct queued_payload *next;
} queued_payload;

typedef struct metadata_entry {
    zend_string *key;
    zend_string *value;
    bool trailing;
    struct metadata_entry *next;
} metadata_entry;

typedef struct _grpc_call grpc_call;

typedef void (*grpc_call_payload_copy_observer)(grpc_call *client, uint64_t elapsed_us);
typedef void (*grpc_call_message_ready_observer)(grpc_call *client, uint64_t ready_abs_us);
typedef void (*grpc_call_payload_queued_observer)(grpc_call *client);
typedef void (*grpc_call_payload_delivered_observer)(grpc_call *client, uint64_t ready_abs_us, uint64_t callback_started_abs_us, uint64_t elapsed_us);
typedef int (*grpc_call_queue_limit_observer)(grpc_call *client);

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
typedef struct {
    size_t data_read_length_calls;
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
    bool read_ahead_delivery;
    size_t read_ahead_max_messages;
    size_t read_ahead_max_bytes;
    size_t call_max_response_queue_count;
    size_t call_max_response_queue_bytes;
    uint64_t call_response_queue_wait_us;
    uint64_t call_max_response_queue_wait_us;
    bool compact_response_buffer;
    size_t response_compact_threshold;
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
} grpc_bench_call;
#endif

struct _grpc_call {
    int fd;
    h2_channel *channel;
    int32_t stream_id;
    bool stream_closed;
    int grpc_status;
    zend_string *grpc_message;
    uint32_t stream_error_code;
    int http_status;
    bool compressed_response_seen;
    bool response_message_too_large;
    bool malformed_response_frame;
    bool metadata_too_large;
    bool discard_response_body;
    bool invalid_grpc_status;
    size_t max_receive_message_bytes;
    size_t bytes_sent;
    size_t bytes_received;
    size_t data_read_calls;
    size_t data_recv_calls;
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
    bool timed_out;
    int last_io_errno;
    int last_ssl_error;
    char last_io_error_detail[256];
    uint64_t deadline_abs_us;
    bool decode_response_incrementally;
    bool direct_response_payload;
    bool queue_response_payloads;
    queued_payload *response_queue_head;
    queued_payload *response_queue_tail;
    size_t response_queue_count;
    size_t response_queue_bytes;
    zend_fcall_info *payload_callback_fci;
    zend_fcall_info_cache *payload_callback_fcc;
    grpc_call_payload_copy_observer observe_payload_copy;
    grpc_call_message_ready_observer observe_message_ready;
    grpc_call_payload_queued_observer observe_payload_queued;
    grpc_call_payload_delivered_observer observe_payload_delivered;
    grpc_call_queue_limit_observer flush_queue_if_limited;
    metadata_entry *metadata_head;
    metadata_entry *metadata_tail;
    size_t metadata_entry_count;
    size_t metadata_bytes;
    size_t response_parse_offset;
    uint8_t response_header_buf[5];
    size_t response_header_len;
    uint32_t response_payload_len;
    size_t response_payload_offset;
    bool response_current_compressed;
    zend_string *response_payload;
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    grpc_bench_call bench;
#endif
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
};

static uint64_t monotonic_us(void);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
#endif
static int parse_grpc_status_value(const uint8_t *value, size_t valuelen);
static int process_response_data_direct(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len);
static int validate_response_message_lengths(nghttp2_session *session, grpc_call *client, const uint8_t *data, size_t len);
static int enqueue_response_payload(grpc_call *client, zend_string *payload);
static int deliver_response_payload(grpc_call *client, zend_string *payload, uint64_t ready_abs_us);
static int deliver_queued_response_payloads(grpc_call *client);
static void free_queued_response_payloads(grpc_call *client);
static int add_metadata_entry(grpc_call *client, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
static void free_metadata_entries(grpc_call *client);
static void add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *client, bool trailing);
static void cleanup_grpc_call(grpc_call *client);
static bool channel_usable(h2_channel *channel);
static int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us);
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);
static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data);

typedef struct {
    nghttp2_nv *nva;
    size_t len;
    size_t capacity;
    zend_string **value_strings;
    size_t value_count;
} h2_request_headers;

static size_t count_custom_header_values(zval *headers_zv);
static void init_request_headers(h2_request_headers *headers, size_t custom_values);
static void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen);
static int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);
static void free_request_headers(h2_request_headers *headers);
static int set_socket_timeout_us(int fd, zend_long timeout_us);
static zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us);
static void mark_channel_dead(h2_channel *channel, int error_code);
static void discard_persistent_channel(const char *key, size_t key_len, h2_channel *channel);
static bool preflight_persistent_channel(h2_channel *channel);
static void cancel_active_stream(h2_stream *stream, uint32_t error_code);

struct _h2_channel {
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
    bool detached_from_cache;
    grpc_call *active_call_owner;
    h2_stream *active_stream_owner;
    int last_error;
    int last_io_errno;
    int last_ssl_error;
    long tls_verify_result;
    char last_error_detail[256];
    char negotiated_protocol[16];
    uint32_t last_goaway_error_code;
    int32_t last_goaway_stream_id;
};

struct _h2_stream {
    h2_channel *channel;
    grpc_call client;
    zend_string *request;
    char *recv_buf;
    size_t recv_buf_len;
    bool completed;
    bool cancelled;
};

static int le_h2_stream;

ZEND_BEGIN_MODULE_GLOBALS(grpc_native)
    HashTable persistent_channels;
    bool persistent_channels_initialized;
ZEND_END_MODULE_GLOBALS(grpc_native)

ZEND_DECLARE_MODULE_GLOBALS(grpc_native)

#define GRPC_NATIVE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(grpc_native, v)

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
        zend_throw_exception(NULL, "native channel already has an active stream", 0);
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
    nghttp2_session_set_user_data(channel->session, &client);
    channel->busy = true;
    channel->active_call_owner = &client;
    init_request_headers(&request_headers, count_custom_header_values(headers_zv));
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
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && client.deadline_abs_us > 0;
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
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&client);
            zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
            return FAILURE;
        }
        client.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;
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
    h2_channel *channel;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;

    ZEND_PARSE_PARAMETERS_START(5, 13)
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
    ZEND_PARSE_PARAMETERS_END();

    if (!GRPC_NATIVE_G(persistent_channels_initialized)) {
        zend_throw_exception(NULL, "persistent channel cache is not initialized", 0);
        RETURN_THROWS();
    }

    channel = zend_hash_str_find_ptr(&GRPC_NATIVE_G(persistent_channels), key, key_len);
    if (channel != NULL && !channel_usable(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
        channel = NULL;
    }
    if (channel != NULL && !preflight_persistent_channel(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
        channel = NULL;
    }

    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    if (channel == NULL) {
        channel = create_h2_channel(host, port, authority, authority_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, true, deadline_abs_us, error_detail, sizeof(error_detail), &error_message);
        if (channel == NULL) {
            zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
            RETURN_THROWS();
        }
        zend_hash_str_update_ptr(&GRPC_NATIVE_G(persistent_channels), key, key_len, channel);
    } else {
        persistent_reused = true;
    }

    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "native transport deadline exceeded", 0);
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

    ZEND_PARSE_PARAMETERS_START(5, 13)
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
    ZEND_PARSE_PARAMETERS_END();

    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        RETURN_THROWS();
    }
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    channel = get_persistent_channel(key, key_len, host, port, authority, authority_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "native channel already has an active stream", 0);
        RETURN_THROWS();
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "native transport deadline exceeded", 0);
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
    init_request_headers(&request_headers, count_custom_header_values(headers_zv));
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
        mark_channel_dead(channel, rv);
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        discard_persistent_channel(key, key_len, channel);
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
    add_assoc_long(return_value, "max_receive_message_length", client->max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) client->max_receive_message_bytes);
    add_assoc_bool(return_value, "timed_out", client->timed_out);
    add_assoc_bool(return_value, "cancelled", stream->cancelled);
    add_assoc_long(return_value, "body_bytes", 0);
    add_assoc_long(return_value, "bytes_sent", client->bytes_sent);
    add_assoc_long(return_value, "bytes_received", client->bytes_received);
    add_assoc_bool(return_value, "channel_dead", stream->channel != NULL ? stream->channel->dead : true);
    add_assoc_bool(return_value, "channel_draining", stream->channel != NULL ? stream->channel->draining : true);
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

    while (client->response_queue_head == NULL && !client->stream_closed && !stream->completed && !client->response_message_too_large && !client->compressed_response_seen && !client->malformed_response_frame) {
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
            stream->completed = true;
            break;
        }
        nread = channel_recv(stream->channel, (uint8_t *) stream->recv_buf, stream->recv_buf_len, client->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && client->deadline_abs_us > 0;
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

    if (client->response_message_too_large || client->compressed_response_seen || client->malformed_response_frame) {
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
    clear_channel_stream_owner(stream);
    add_stream_status(return_value, stream);
}

PHP_FUNCTION(grpc_lite_stream_cancel)
{
    zval *stream_zv = NULL;
    h2_stream *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (h2_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_stream", le_h2_stream);
    if (stream != NULL && !stream->completed && channel_owned_by_stream(stream->channel, stream) && channel_usable(stream->channel)) {
        stream->cancelled = true;
        stream->completed = true;
        stream->client.grpc_status = 1;
        nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        nghttp2_session_send(stream->channel->session);
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

    if (!GRPC_NATIVE_G(persistent_channels_initialized)) {
        RETURN_FALSE;
    }

    channel = zend_hash_str_find_ptr(&GRPC_NATIVE_G(persistent_channels), key, key_len);
    if (channel == NULL) {
        RETURN_FALSE;
    }

    discard_persistent_channel(key, key_len, channel);
    RETURN_TRUE;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
#include "grpc_bench.c"
#endif

PHP_GINIT_FUNCTION(grpc_native)
{
#if defined(COMPILE_DL_GRPC) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_init(&grpc_native_globals->persistent_channels, 8, NULL, NULL, 1);
    grpc_native_globals->persistent_channels_initialized = true;
}

PHP_GSHUTDOWN_FUNCTION(grpc_native)
{
    h2_channel *channel;

    if (!grpc_native_globals->persistent_channels_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&grpc_native_globals->persistent_channels, channel) {
        destroy_h2_channel(channel);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&grpc_native_globals->persistent_channels);
    grpc_native_globals->persistent_channels_initialized = false;
}

PHP_MINIT_FUNCTION(grpc_native)
{
    le_h2_stream = zend_register_list_destructors_ex(h2_stream_dtor, NULL, "grpc_lite_stream", module_number);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(grpc_native)
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

static const zend_function_entry grpc_native_functions[] = {
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
    grpc_native_functions,
    PHP_MINIT(grpc_native),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(grpc_native),
    "0.1.0",
    ZEND_MODULE_GLOBALS(grpc_native),
    PHP_GINIT(grpc_native),
    PHP_GSHUTDOWN(grpc_native),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_GRPC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(grpc)
#endif
