#ifndef PHP_GRPC_LITE_CALL_H
#define PHP_GRPC_LITE_CALL_H

#include "surface.h"

typedef struct _h2_connection h2_connection;
typedef struct persistent_connection_entry persistent_connection_entry;
typedef struct server_streaming_call_state server_streaming_call_state;

typedef struct queued_payload {
    zend_string *payload;
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
    bool compact_response_buffer;
    size_t response_compact_threshold;
    zend_long call_decoded_messages;
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
    zend_string *method_path;
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
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
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
#endif
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
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
#endif
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

#endif /* PHP_GRPC_LITE_CALL_H */
