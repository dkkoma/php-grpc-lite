#ifndef PHP_GRPC_LITE_INTERNAL_H
#define PHP_GRPC_LITE_INTERNAL_H

/*
 * Private implementation header for php-grpc-lite ext/grpc.
 * Not installed. Not a public C API.
 */

#include <php.h>
#include <php_ini.h>
#include <ext/standard/info.h>
#include <ext/standard/base64.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp2/nghttp2.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
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

typedef struct _h2_connection h2_connection;
typedef struct persistent_connection_entry persistent_connection_entry;
typedef struct server_streaming_call_state server_streaming_call_state;

static zend_class_entry *grpc_ce_channel;
static zend_class_entry *grpc_ce_call;
static zend_class_entry *grpc_ce_channel_credentials;
static zend_class_entry *grpc_ce_call_credentials;
static zend_class_entry *grpc_ce_timeval;
static zend_object_handlers grpc_channel_handlers;
static zend_object_handlers grpc_call_handlers;
static zend_object_handlers grpc_channel_credentials_handlers;
static zend_object_handlers grpc_call_credentials_handlers;
static zend_object_handlers grpc_timeval_handlers;

#define GRPC_STATUS_OK 0
#define GRPC_STATUS_CANCELLED 1
#define GRPC_STATUS_UNKNOWN 2
#define GRPC_STATUS_INVALID_ARGUMENT 3
#define GRPC_STATUS_DEADLINE_EXCEEDED 4
#define GRPC_STATUS_NOT_FOUND 5
#define GRPC_STATUS_ALREADY_EXISTS 6
#define GRPC_STATUS_PERMISSION_DENIED 7
#define GRPC_STATUS_RESOURCE_EXHAUSTED 8
#define GRPC_STATUS_FAILED_PRECONDITION 9
#define GRPC_STATUS_ABORTED 10
#define GRPC_STATUS_OUT_OF_RANGE 11
#define GRPC_STATUS_UNIMPLEMENTED 12
#define GRPC_STATUS_INTERNAL 13
#define GRPC_STATUS_UNAVAILABLE 14
#define GRPC_STATUS_DATA_LOSS 15
#define GRPC_STATUS_UNAUTHENTICATED 16

#define GRPC_OP_SEND_INITIAL_METADATA 0
#define GRPC_OP_SEND_MESSAGE 1
#define GRPC_OP_SEND_CLOSE_FROM_CLIENT 2
#define GRPC_OP_RECV_INITIAL_METADATA 4
#define GRPC_OP_RECV_MESSAGE 5
#define GRPC_OP_RECV_STATUS_ON_CLIENT 6

typedef enum {
    GRPC_LITE_CREDENTIALS_INSECURE = 0,
    GRPC_LITE_CREDENTIALS_SSL = 1,
    GRPC_LITE_CREDENTIALS_DEFAULT = 2
} grpc_lite_credentials_type;

typedef enum {
    GRPC_LITE_BACKEND_HTTP2 = 0,
    GRPC_LITE_BACKEND_FRANKEN_GO = 1
} grpc_lite_backend_type;

typedef struct {
    grpc_lite_credentials_type type;
    zend_string *root_certs;
    zend_string *private_key;
    zend_string *cert_chain;
    zend_object std;
} grpc_lite_channel_credentials_obj;

typedef struct {
    zval callback;
    zend_object std;
} grpc_lite_call_credentials_obj;

typedef struct {
    zend_string *target;
    zend_string *host;
    zend_long port;
    zend_string *authority;
    zend_string *tls_verify_name;
    zend_string *primary_user_agent;
    zend_long max_receive_message_length;
    size_t max_response_metadata_bytes;
    zval credentials;
    zval franken_channel;
    grpc_lite_backend_type backend;
    bool initialized;
    zend_object std;
} grpc_lite_channel_obj;

typedef struct {
    zval channel;
    zend_string *method;
    zend_long deadline_us;
    zval credentials;
    zend_string *request_payload;
    zend_string *unary_response_payload;
    zval metadata;
    zval server_streaming_resource;
    zval franken_server_streaming_call;
    zval initial_metadata;
    zval trailing_metadata;
    zval status;
    bool sent;
    bool unary_performed;
    bool server_streaming_opened;
    bool initial_metadata_ready;
    bool status_ready;
    bool cancelled;
    bool initialized;
    zend_object std;
} grpc_lite_call_obj;

typedef struct {
    zend_long microseconds;
    zend_object std;
} grpc_lite_timeval_obj;

static inline grpc_lite_channel_credentials_obj *grpc_lite_channel_credentials_fetch(zend_object *obj)
{
    return (grpc_lite_channel_credentials_obj *) ((char *) obj - XtOffsetOf(grpc_lite_channel_credentials_obj, std));
}

static inline grpc_lite_call_credentials_obj *grpc_lite_call_credentials_fetch(zend_object *obj)
{
    return (grpc_lite_call_credentials_obj *) ((char *) obj - XtOffsetOf(grpc_lite_call_credentials_obj, std));
}

static inline grpc_lite_channel_obj *grpc_lite_channel_fetch(zend_object *obj)
{
    return (grpc_lite_channel_obj *) ((char *) obj - XtOffsetOf(grpc_lite_channel_obj, std));
}

static inline grpc_lite_call_obj *grpc_lite_call_fetch(zend_object *obj)
{
    return (grpc_lite_call_obj *) ((char *) obj - XtOffsetOf(grpc_lite_call_obj, std));
}

static inline grpc_lite_timeval_obj *grpc_lite_timeval_fetch(zend_object *obj)
{
    return (grpc_lite_timeval_obj *) ((char *) obj - XtOffsetOf(grpc_lite_timeval_obj, std));
}

#define Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(zv) grpc_lite_channel_credentials_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CALL_CREDENTIALS_P(zv) grpc_lite_call_credentials_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CHANNEL_P(zv) grpc_lite_channel_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CALL_P(zv) grpc_lite_call_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_TIMEVAL_P(zv) grpc_lite_timeval_fetch(Z_OBJ_P((zv)))

PHP_METHOD(Call, __construct);
PHP_METHOD(Call, setCredentials);
PHP_METHOD(Call, cancel);
PHP_METHOD(Call, getPeer);

ZEND_BEGIN_ARG_INFO_EX(arginfo_no_args, 0, 0, 0)
ZEND_END_ARG_INFO()

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

typedef struct {
    int code;
    zend_string *details;
} grpc_lite_status_result;

typedef struct {
    zend_string *body;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
} grpc_lite_unary_result;

typedef struct {
    bool done;
    zend_string *payload;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
} grpc_lite_streaming_next_result;

typedef void (*grpc_call_payload_copy_observer)(grpc_call *call, uint64_t elapsed_us);
typedef void (*grpc_call_message_ready_observer)(grpc_call *call, uint64_t ready_abs_us);
typedef void (*grpc_call_payload_queued_observer)(grpc_call *call);
typedef void (*grpc_call_payload_delivered_observer)(grpc_call *call, uint64_t ready_abs_us, uint64_t callback_started_abs_us, uint64_t elapsed_us);
typedef int (*grpc_call_queue_limit_observer)(grpc_call *call);

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
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    int fd;
#endif
    h2_connection *connection;
    grpc_call *next_active_stream;
    int32_t stream_id;
    bool stream_registered;
    bool connection_owned;
    bool stream_closed;
    int grpc_status;
    zend_string *grpc_message;
    uint32_t stream_error_code;
    bool stream_reset_seen;
    bool stream_refused_seen;
    int http_status;
    bool compressed_response_seen;
    bool response_message_too_large;
    bool malformed_response_frame;
    bool metadata_too_large;
    bool content_type_seen;
    bool invalid_content_type;
    bool unsupported_response_encoding;
    bool response_queue_limit_exceeded;
    zend_string *content_type;
    zend_string *grpc_encoding;
    bool discard_response_body;
    bool invalid_grpc_status;
    bool grpc_status_seen;
    bool initial_grpc_status_seen;
    bool initial_headers_end_stream;
    size_t response_message_count;
    size_t max_response_messages;
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
    size_t max_response_metadata_bytes;
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

typedef struct {
    nghttp2_nv *nva;
    size_t len;
    size_t capacity;
    zend_string **name_strings;
    size_t name_count;
    zend_string **value_strings;
    size_t value_count;
} h2_request_headers;

struct _h2_connection {
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
    bool detached_from_cache;
    size_t active_stream_count;
    size_t stream_owner_count;
    grpc_call *active_streams;
    grpc_call *current_io_call;
    grpc_call *current_read_call;
    uint64_t current_write_deadline_abs_us;
    bool current_write_timed_out;
    uint64_t setup_deadline_abs_us;
    bool setup_timed_out;
    int last_error;
    int last_io_errno;
    int last_ssl_error;
    long tls_verify_result;
    char last_error_detail[256];
    char negotiated_protocol[16];
    uint32_t last_goaway_error_code;
    int32_t last_goaway_stream_id;
};

struct persistent_connection_entry {
    h2_connection *connection;
    zend_string *host_identity;
    zend_string *authority_identity;
    zend_string *tls_verify_name_identity;
    zend_string *root_certs_identity;
    zend_string *cert_chain_identity;
    zend_string *private_key_identity;
    size_t host_len;
    zend_ulong host_hash;
    zend_long port;
    size_t authority_len;
    zend_ulong authority_hash;
    bool use_tls;
    size_t tls_verify_name_len;
    zend_ulong tls_verify_name_hash;
    size_t root_certs_len;
    zend_ulong root_certs_hash;
    size_t cert_chain_len;
    zend_ulong cert_chain_hash;
    size_t private_key_len;
    zend_ulong private_key_hash;
};

struct server_streaming_call_state {
    grpc_call call;
    zend_string *request;
    zend_string *path;
    zval metadata;
    char *recv_buf;
    size_t recv_buf_len;
    uint64_t start_unix_nanos;
    uint64_t total_started_us;
    uint64_t setup_us;
    uint64_t submit_us;
    uint64_t initial_send_us;
    uint64_t recv_loop_us;
    uint64_t delivered_messages;
    uint64_t delivered_payload_bytes;
    bool completed;
    bool cancelled;
    bool persistent_reused;
};


static void destroy_h2_connection(h2_connection *connection);
static void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection);
static void detach_persistent_connection_by_ptr(h2_connection *connection);
static bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *state);
static bool connection_owned_by_call(h2_connection *connection, grpc_call *call);
static void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *state);
static void clear_connection_call_owner(h2_connection *connection, grpc_call *call);
static void destroy_detached_connection_if_unowned(h2_connection *connection);
static void mark_grpc_call_stream_registration_failed(h2_connection *connection, grpc_call *call);
static int send_pending_h2_frames(h2_connection *connection, grpc_call *call);
static void cancel_active_server_streaming_call_state(server_streaming_call_state *state, uint32_t error_code);
static void destroy_server_streaming_call_state(server_streaming_call_state *state);
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
static size_t effective_max_response_metadata_bytes(zend_long soft_limit, zend_long hard_limit);
static int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us);
static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len);
static int configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len);
static int configure_tls_connection(h2_connection *connection, const char *host, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us);
static ssize_t connection_send(grpc_call *call, const uint8_t *data, size_t length);
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
static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
static uint64_t monotonic_us(void);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
#endif
static int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen);
static bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen);
static int grpc_lite_hex_value(unsigned char ch);
static size_t grpc_lite_format_timeout_us(char *buffer, size_t buffer_len, long timeout_us);
static zend_string *grpc_protocol_decode_message(const uint8_t *value, size_t valuelen);
static int grpc_lite_status_code_from_call(grpc_call *call, bool cancelled);
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
static int enqueue_response_payload(nghttp2_session *session, grpc_call *call, zend_string *payload);
static int deliver_response_payload(grpc_call *call, zend_string *payload, uint64_t ready_abs_us);
static int deliver_queued_response_payloads(grpc_call *call);
static void free_queued_response_payloads(grpc_call *call);
static void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call);
static int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
static void grpc_protocol_free_response_metadata_entries(grpc_call *call);
static void grpc_protocol_copy_metadata_map(zval *metadata, grpc_call *call, bool trailing);
static void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing);
static void resolve_grpc_call_status(grpc_call *call, bool cancelled, grpc_lite_status_result *result);
static void add_status_result_to_return(zval *return_value, grpc_lite_status_result *status);
static void cleanup_grpc_call(grpc_call *call);

static void grpc_lite_unary_result_dtor(grpc_lite_unary_result *result);
static void grpc_lite_streaming_next_result_dtor(grpc_lite_streaming_next_result *result);
static int grpc_lite_unary_call_perform_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, uint64_t deadline_abs_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, grpc_lite_unary_result *result);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static int grpc_lite_unary_call_perform_diagnostic_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, zval *return_value);
#endif
static int server_streaming_call_open_resource(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, zend_long max_receive_message_length, size_t max_response_metadata_bytes, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, zval *return_value, grpc_lite_status_result *setup_failure);
static int server_streaming_call_next_resource(zval *server_streaming_resource_zv, grpc_lite_streaming_next_result *result);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static int server_streaming_call_next_resource_diagnostic(zval *server_streaming_resource_zv, zval *return_value);
#endif
static int server_streaming_call_cancel_resource(zval *server_streaming_resource_zv);
static int grpc_lite_channel_key(grpc_lite_channel_obj *channel, zend_string **key);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static void grpc_lite_diagnostic_add_unary_result(zval *diagnostic_result, const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status, uint64_t start_unix_nanos, uint64_t total_us, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused);
static void grpc_lite_diagnostic_add_server_streaming_status(zval *diagnostic_result, server_streaming_call_state *state, grpc_lite_status_result *status);
#endif

ZEND_BEGIN_MODULE_GLOBALS(grpc_lite)
    HashTable persistent_connections;
    bool persistent_connections_initialized;
    zend_string *default_roots_pem;
    zend_long http2_stream_window_size;
    zend_long http2_connection_window_size;
    zend_long server_streaming_read_ahead_max_messages;
    zend_long server_streaming_read_ahead_max_bytes;
    char *backend;
ZEND_END_MODULE_GLOBALS(grpc_lite)

ZEND_EXTERN_MODULE_GLOBALS(grpc_lite)

#define PHP_GRPC_LITE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(grpc_lite, v)

#endif /* PHP_GRPC_LITE_INTERNAL_H */
