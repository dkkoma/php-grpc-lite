#ifndef PHP_GRPC_LITE_GRPC_EXCHANGE_STATE_H
#define PHP_GRPC_LITE_GRPC_EXCHANGE_STATE_H

#include "common.h"
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
#include "diagnostic/bench_call.h"
#endif

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

struct _grpc_call {
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    int fd;
#endif
    h2_connection *connection;
    grpc_call *next_active_stream;
    zend_string *method_path;
    int32_t stream_id;
    uint32_t retry_attempt;
    bool stream_registered;
    bool connection_owned;
    bool stream_closed;
    int grpc_status;
    zend_string *grpc_message;
    uint32_t stream_error_code;
    bool stream_reset_seen;
    /* This side submitted RST_STREAM for the stream (deadline expiry / user
     * cancel); a mid-message stream end is then cancellation semantics, not a
     * malformed (truncated) response. */
    bool locally_cancelled;
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
    bool trailing_headers_seen;
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
    zend_string *response_payload;
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    grpc_bench_call bench;
#endif
    /* Raw wire-byte accumulator for the legacy (non-direct-decode) data path.
     * Production unary and server streaming both use the direct decode path
     * (response_queue), so this stays empty there; only the BENCH diagnostic
     * raw client still appends to it. */
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

#endif /* PHP_GRPC_LITE_GRPC_EXCHANGE_STATE_H */
