/* Pure HTTP/2 transport helpers shared by the PHP extension and C unit tests. */

#ifndef PHP_GRPC_LITE_TRANSPORT_CORE_C
#define PHP_GRPC_LITE_TRANSPORT_CORE_C

#include "internal.h"
#ifdef snprintf
#undef snprintf
#endif

zend_ulong hash_bytes(const char *data, size_t data_len)
{
    zend_ulong hash = (zend_ulong) 1469598103934665603ULL;
    size_t index;

    if (data == NULL || data_len == 0) {
        return 0;
    }
    for (index = 0; index < data_len; index++) {
        hash ^= (unsigned char) data[index];
        hash *= (zend_ulong) 1099511628211ULL;
    }
    return hash;
}

void build_authority(char *buffer, size_t buffer_len, const char *host, zend_long port, const char *authority, size_t authority_len)
{
    if (authority != NULL && authority_len > 0) {
        size_t copy_len = authority_len < buffer_len - 1 ? authority_len : buffer_len - 1;
        memcpy(buffer, authority, copy_len);
        buffer[copy_len] = '\0';
        return;
    }

    snprintf(buffer, buffer_len, "%s:%ld", host, port);
}

size_t effective_max_receive_message_bytes(zend_long max_receive_message_length)
{
    if (max_receive_message_length == -1) {
        return SIZE_MAX;
    }
    if (max_receive_message_length > 0) {
        return (size_t) max_receive_message_length;
    }
    return GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES;
}

uint32_t effective_http2_window_size(zend_long configured)
{
    if (configured < GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE;
    }
    if (configured > GRPC_LITE_HTTP2_MAX_WINDOW_SIZE) {
        return GRPC_LITE_HTTP2_MAX_WINDOW_SIZE;
    }
    return (uint32_t) configured;
}

uint32_t effective_http2_max_frame_size(zend_long configured)
{
    if (configured < GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE) {
        return GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE;
    }
    if (configured > GRPC_LITE_HTTP2_MAX_FRAME_SIZE) {
        return GRPC_LITE_HTTP2_MAX_FRAME_SIZE;
    }
    return (uint32_t) configured;
}

uint32_t effective_http2_max_header_list_size(zend_long configured)
{
    if (configured < 0) {
        return 0;
    }
    if ((zend_ulong) configured > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t) configured;
}

size_t effective_max_response_metadata_bytes(zend_long soft_limit, zend_long hard_limit)
{
    if (hard_limit >= 0) {
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

const char *validate_channel_inputs(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len)
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
        if (authority_len >= sizeof(((h2_connection *) 0)->authority) || contains_authority_forbidden_char(authority, authority_len)) {
            return "invalid gRPC authority";
        }
    } else {
        port_len = snprintf(port_buf, sizeof(port_buf), "%ld", port);
        if (port_len < 0 || host_len + 1 + (size_t) port_len >= sizeof(((h2_connection *) 0)->authority)) {
            return "gRPC authority is too long";
        }
    }
    if (tls_verify_name_len > 0 && contains_authority_forbidden_char(tls_verify_name, tls_verify_name_len)) {
        return "invalid TLS verify name";
    }

    return NULL;
}

#endif
