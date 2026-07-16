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

/* Size the write coalesce buffer from the effective SETTINGS_MAX_FRAME_SIZE:
 * a full-size DATA chunk arrives from nghttp2 as 9 + max_frame_size bytes, so
 * the buffer must hold several of them or every full DATA frame bypasses
 * coalescing and becomes its own SSL_write/send. Clamped so an oversized
 * grpc_lite.http2_max_frame_size (up to 16MB) cannot pin 4x that per
 * persistent connection; frames larger than the cap fall back to a direct
 * write, which is already an amortized-cost path. */
size_t h2_write_coalesce_capacity_for_max_frame_size(uint32_t max_frame_size)
{
    size_t capacity = 4 * ((size_t) max_frame_size + 9);
    if (capacity < GRPC_LITE_H2_WRITE_COALESCE_MIN_CAPACITY) {
        return GRPC_LITE_H2_WRITE_COALESCE_MIN_CAPACITY;
    }
    if (capacity > GRPC_LITE_H2_WRITE_COALESCE_MAX_CAPACITY) {
        return GRPC_LITE_H2_WRITE_COALESCE_MAX_CAPACITY;
    }
    return capacity;
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

bool grpc_response_header_budget_account_field(size_t *entry_count, size_t *bytes, size_t max_bytes, size_t namelen, size_t valuelen)
{
    size_t field_bytes;

    if (namelen > SIZE_MAX - valuelen) {
        return false;
    }
    field_bytes = namelen + valuelen;
    if (*entry_count >= GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES
        || field_bytes > max_bytes
        || *bytes > max_bytes - field_bytes) {
        return false;
    }

    (*entry_count)++;
    *bytes += field_bytes;
    return true;
}

#define GRPC_LITE_HTTP2_FRAME_TYPE_HEADERS 0x1
#define GRPC_LITE_HTTP2_FRAME_TYPE_CONTINUATION 0x9
#define GRPC_LITE_HTTP2_FLAG_END_HEADERS 0x4

static void grpc_h2_receive_boundary_complete_frame(grpc_h2_receive_boundary_state *state)
{
    bool end_headers = (state->frame_flags & GRPC_LITE_HTTP2_FLAG_END_HEADERS) != 0;

    if (state->frame_type == GRPC_LITE_HTTP2_FRAME_TYPE_HEADERS) {
        state->header_block_in_flight = !end_headers;
    } else if (state->frame_type == GRPC_LITE_HTTP2_FRAME_TYPE_CONTINUATION
        && state->header_block_in_flight
        && end_headers) {
        state->header_block_in_flight = false;
    }

    state->frame_payload_remaining = 0;
    state->frame_type = 0;
    state->frame_flags = 0;
}

void grpc_h2_receive_boundary_state_reset(grpc_h2_receive_boundary_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void grpc_h2_receive_boundary_state_consume(grpc_h2_receive_boundary_state *state, const uint8_t *data, size_t len)
{
    if (state == NULL || data == NULL) {
        return;
    }

    while (len > 0) {
        if (state->frame_payload_remaining > 0) {
            size_t consumed = len < state->frame_payload_remaining
                ? len
                : (size_t) state->frame_payload_remaining;
            state->frame_payload_remaining -= (uint32_t) consumed;
            data += consumed;
            len -= consumed;
            if (state->frame_payload_remaining == 0) {
                grpc_h2_receive_boundary_complete_frame(state);
            }
            continue;
        }

        if (state->frame_header_bytes < GRPC_LITE_HTTP2_FRAME_HEADER_SIZE) {
            size_t needed = GRPC_LITE_HTTP2_FRAME_HEADER_SIZE - state->frame_header_bytes;
            size_t consumed = len < needed ? len : needed;
            memcpy(state->frame_header + state->frame_header_bytes, data, consumed);
            state->frame_header_bytes += consumed;
            data += consumed;
            len -= consumed;
            if (state->frame_header_bytes < GRPC_LITE_HTTP2_FRAME_HEADER_SIZE) {
                continue;
            }

            state->frame_payload_remaining = ((uint32_t) state->frame_header[0] << 16)
                | ((uint32_t) state->frame_header[1] << 8)
                | (uint32_t) state->frame_header[2];
            state->frame_type = state->frame_header[3];
            state->frame_flags = state->frame_header[4];
            state->frame_header_bytes = 0;
            if (state->frame_payload_remaining == 0) {
                grpc_h2_receive_boundary_complete_frame(state);
            }
        }
    }
}

bool grpc_h2_receive_allows_reuse_after_abandonment(const grpc_h2_receive_boundary_state *state)
{
    /* A call may leave its connection reusable only when the receive parser
     * is between complete frames and is not waiting for continuation of an
     * inbound response header block. This byte-level invariant also covers
     * states before nghttp2 can emit any semantic header callback. */
    return state != NULL
        && state->frame_header_bytes == 0
        && state->frame_payload_remaining == 0
        && !state->header_block_in_flight;
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
