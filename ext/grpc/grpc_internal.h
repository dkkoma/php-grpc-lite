#ifndef PHP_GRPC_LITE_INTERNAL_H
#define PHP_GRPC_LITE_INTERNAL_H

/*
 * Private implementation header for php-grpc-lite ext/grpc.
 * Not installed. Not a public C API.
 */

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
    bool content_type_seen;
    bool invalid_content_type;
    bool unsupported_response_encoding;
    bool discard_response_body;
    bool invalid_grpc_status;
    bool grpc_status_seen;
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

struct _h2_channel {
    int fd;
    bool tls;
    bool persistent;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    char authority[512];
    size_t host_len;
    zend_ulong host_hash;
    zend_long port;
    size_t authority_len;
    zend_ulong authority_hash;
    size_t tls_verify_name_len;
    zend_ulong tls_verify_name_hash;
    size_t root_certs_len;
    zend_ulong root_certs_hash;
    size_t cert_chain_len;
    zend_ulong cert_chain_hash;
    size_t private_key_len;
    zend_ulong private_key_hash;
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

ZEND_BEGIN_MODULE_GLOBALS(grpc_lite)
    HashTable persistent_channels;
    bool persistent_channels_initialized;
ZEND_END_MODULE_GLOBALS(grpc_lite)

ZEND_EXTERN_MODULE_GLOBALS(grpc_lite)

#define PHP_GRPC_LITE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(grpc_lite, v)

#endif /* PHP_GRPC_LITE_INTERNAL_H */
