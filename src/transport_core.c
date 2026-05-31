/* Pure HTTP/2 transport helpers shared by the PHP extension and C unit tests. */

#ifndef PHP_GRPC_LITE_TRANSPORT_CORE_C
#define PHP_GRPC_LITE_TRANSPORT_CORE_C

#include "transport_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef snprintf
#undef snprintf
#endif

void build_authority(char *buffer, size_t buffer_len, const char *host, int64_t port, const char *authority, size_t authority_len)
{
    if (authority != NULL && authority_len > 0) {
        size_t copy_len = authority_len < buffer_len - 1 ? authority_len : buffer_len - 1;
        memcpy(buffer, authority, copy_len);
        buffer[copy_len] = '\0';
        return;
    }

    snprintf(buffer, buffer_len, "%s:%" PRId64, host, port);
}

size_t effective_max_receive_message_bytes(int64_t max_receive_message_length)
{
    if (max_receive_message_length == -1) {
        return SIZE_MAX;
    }
    if (max_receive_message_length > 0) {
        return (size_t) max_receive_message_length;
    }
    return GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES;
}

uint32_t effective_http2_window_size(int64_t configured)
{
    if (configured < GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE;
    }
    if (configured > GRPC_LITE_HTTP2_MAX_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_MAX_WINDOW_SIZE;
    }
    return (uint32_t) configured;
}

uint32_t effective_http2_max_frame_size(int64_t configured)
{
    if (configured < GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE) {
        return GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE;
    }
    if (configured > GRPC_LITE_HTTP2_MAX_FRAME_SIZE) {
        return GRPC_LITE_HTTP2_MAX_FRAME_SIZE;
    }
    return (uint32_t) configured;
}

uint32_t effective_http2_max_header_list_size(int64_t configured)
{
    if (configured < 0) {
        return 0;
    }
    if (configured > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t) configured;
}

size_t effective_max_response_metadata_bytes(int64_t soft_limit, int64_t hard_limit)
{
    if (hard_limit >= 0) {
        if ((uint64_t) hard_limit > (uint64_t) SIZE_MAX) {
            return SIZE_MAX;
        }
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

bool contains_nul_or_control(const char *value, size_t value_len)
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

bool contains_authority_forbidden_char(const char *value, size_t value_len)
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

const char *validate_grpc_path(const char *path, size_t path_len)
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

const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, int64_t port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len)
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
        if (authority_len >= GRPC_LITE_AUTHORITY_BUFFER_SIZE || contains_authority_forbidden_char(authority, authority_len)) {
            return "invalid gRPC authority";
        }
    } else {
        port_len = snprintf(port_buf, sizeof(port_buf), "%" PRId64, port);
        if (port_len < 0 || host_len + 1 + (size_t) port_len >= GRPC_LITE_AUTHORITY_BUFFER_SIZE) {
            return "gRPC authority is too long";
        }
    }
    if (tls_verify_name_len > 0 && contains_authority_forbidden_char(tls_verify_name, tls_verify_name_len)) {
        return "invalid TLS verify name";
    }

    return NULL;
}

#endif
