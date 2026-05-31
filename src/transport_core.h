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
#define GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES 128
#define GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES (64 * 1024)
#define GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_MESSAGES 32
#define GRPC_LITE_DEFAULT_SERVER_STREAMING_READ_AHEAD_BYTES (8 * 1024 * 1024)
#define GRPC_LITE_DEFAULT_METADATA_HARD_BYTES (16 * 1024)
#define GRPC_LITE_MAX_PERSISTENT_CONNECTIONS 128
#define GRPC_LITE_AUTHORITY_BUFFER_SIZE 512

void build_authority(char *buffer, size_t buffer_len, const char *host, int64_t port, const char *authority, size_t authority_len);
size_t effective_max_receive_message_bytes(int64_t max_receive_message_length);
uint32_t effective_http2_window_size(int64_t configured);
uint32_t effective_http2_max_frame_size(int64_t configured);
uint32_t effective_http2_max_header_list_size(int64_t configured);
size_t effective_max_response_metadata_bytes(int64_t soft_limit, int64_t hard_limit);
bool contains_nul_or_control(const char *value, size_t value_len);
bool contains_authority_forbidden_char(const char *value, size_t value_len);
const char *validate_grpc_path(const char *path, size_t path_len);
const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, int64_t port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len);

#endif
