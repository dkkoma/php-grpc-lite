#ifndef PHP_GRPC_LITE_TRANSPORT_CORE_H
#define PHP_GRPC_LITE_TRANSPORT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES (64 * 1024 * 1024)
#define GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE 65535
#define GRPC_LITE_HTTP2_MAX_WINDOW_SIZE 2147483647L
#define GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE 16384
#define GRPC_LITE_HTTP2_MAX_FRAME_SIZE 16777215
#define GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE (64 * 1024)
#define GRPC_LITE_MAX_REQUEST_METADATA_VALUES 256
/* :method, :scheme, :authority, :path, content-type, te, grpc-timeout, user-agent */
#define GRPC_LITE_REQUEST_FIXED_HEADER_COUNT 8
#define GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES 128
#define GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES (64 * 1024)
#define GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_MESSAGES 32
#define GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_BYTES (8 * 1024 * 1024)
#define GRPC_LITE_DEFAULT_METADATA_HARD_BYTES (16 * 1024)
#define GRPC_LITE_MAX_PERSISTENT_CONNECTIONS 128
#define GRPC_LITE_PREFLIGHT_DRAIN_DEFAULT_MAX_BYTES 65536
#define GRPC_LITE_AUTHORITY_BUFFER_SIZE 512
#define GRPC_LITE_H2_WRITE_COALESCE_MIN_CAPACITY 65536
#define GRPC_LITE_H2_WRITE_COALESCE_MAX_CAPACITY (1024 * 1024)
#define GRPC_LITE_HTTP2_FRAME_HEADER_SIZE 9

typedef struct {
    uint8_t frame_header[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE];
    size_t frame_header_bytes;
    uint32_t frame_payload_remaining;
    uint8_t frame_type;
    uint8_t frame_flags;
    bool header_block_in_flight;
} grpc_h2_receive_boundary_state;

void build_authority(char *buffer, size_t buffer_len, const char *host, int64_t port, const char *authority, size_t authority_len);
size_t effective_max_receive_message_bytes(int64_t max_receive_message_length);
uint32_t effective_http2_window_size(int64_t configured);
uint32_t effective_http2_max_frame_size(int64_t configured);
size_t h2_write_coalesce_capacity_for_max_frame_size(uint32_t max_frame_size);
uint32_t effective_http2_max_header_list_size(int64_t configured);
size_t effective_max_response_metadata_bytes(int64_t soft_limit, int64_t hard_limit);
bool grpc_response_header_budget_account_field(size_t *entry_count, size_t *bytes, size_t max_bytes, size_t namelen, size_t valuelen);
/* Connection/session-scoped mirror of bytes handed to
 * nghttp2_session_mem_recv(). Callers account before entering nghttp2 so
 * callbacks observe the current invocation's complete byte boundary; a
 * mem_recv error makes that session terminal. Do not reset this state between
 * calls or iterations on the same HTTP/2 session. */
void grpc_h2_receive_boundary_state_reset(grpc_h2_receive_boundary_state *state);
void grpc_h2_receive_boundary_state_consume(grpc_h2_receive_boundary_state *state, const uint8_t *data, size_t len);
bool grpc_h2_receive_allows_reuse_after_abandonment(const grpc_h2_receive_boundary_state *state);
bool contains_nul_or_control(const char *value, size_t value_len);
bool contains_authority_forbidden_char(const char *value, size_t value_len);
const char *validate_grpc_path(const char *path, size_t path_len);
const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, int64_t port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len);

#endif
