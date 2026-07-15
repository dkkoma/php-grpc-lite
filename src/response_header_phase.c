/* Pure response header-block phase transitions shared by all transports. */

#ifndef PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_C
#define PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_C

#include "response_header_phase.h"

#include <string.h>

void grpc_response_header_phase_reset(grpc_response_header_phase_state *state)
{
    state->block_phase = GRPC_RESPONSE_HEADER_BLOCK_NONE;
    state->final_response_headers_seen = false;
    state->trailers_only_candidate = false;
}

grpc_response_header_block_phase grpc_response_header_phase_begin(grpc_response_header_phase_state *state)
{
    state->block_phase = state->final_response_headers_seen
        ? GRPC_RESPONSE_HEADER_BLOCK_TRAILING
        : GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS;
    state->trailers_only_candidate = false;
    return state->block_phase;
}

grpc_response_header_block_phase grpc_response_header_phase_on_status(grpc_response_header_phase_state *state, int http_status)
{
    if (state->block_phase != GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS) {
        return state->block_phase;
    }

    if (http_status >= 100 && http_status < 200) {
        state->block_phase = GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL;
    } else {
        state->block_phase = GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL;
        state->final_response_headers_seen = true;
    }
    return state->block_phase;
}

grpc_response_header_block_phase grpc_response_header_phase_end(grpc_response_header_phase_state *state)
{
    grpc_response_header_block_phase ended_phase = state->block_phase;
    state->block_phase = GRPC_RESPONSE_HEADER_BLOCK_NONE;
    state->trailers_only_candidate = false;
    return ended_phase;
}

bool grpc_response_header_phase_requires_connection_terminal_on_abandonment(const grpc_response_header_phase_state *state)
{
    /* The normal frame-recv path, plus the rejected-frame fallback for
     * complete invalid HEADERS, synchronously returns each complete block to
     * NONE. Any other phase therefore still owns a connection-global HPACK
     * block which cannot survive losing its live call owner. */
    return state != NULL && state->block_phase != GRPC_RESPONSE_HEADER_BLOCK_NONE;
}

bool grpc_response_header_phase_allows_status_fields(const grpc_response_header_phase_state *state, bool end_stream)
{
    return state != NULL
        && end_stream
        && (state->block_phase == GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL
            || state->block_phase == GRPC_RESPONSE_HEADER_BLOCK_TRAILING);
}

bool grpc_response_header_phase_on_trailers_only_status_field(grpc_response_header_phase_state *state, bool end_stream)
{
    if (!grpc_response_header_phase_allows_status_fields(state, end_stream)
        || state->block_phase != GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL
        || state->trailers_only_candidate) {
        return false;
    }
    state->trailers_only_candidate = true;
    return true;
}

bool grpc_response_header_phase_metadata_is_trailing(const grpc_response_header_phase_state *state)
{
    return state != NULL
        && (state->block_phase == GRPC_RESPONSE_HEADER_BLOCK_TRAILING
            || (state->block_phase == GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL
                && state->trailers_only_candidate));
}

grpc_response_header_field_class grpc_response_header_classify_reported_field(const uint8_t *name, size_t namelen, bool invalid_regular)
{
    if (name == NULL) {
        return GRPC_RESPONSE_HEADER_FIELD_REJECTED;
    }
    if (namelen == 0) {
        return invalid_regular
            ? GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR
            : GRPC_RESPONSE_HEADER_FIELD_REJECTED;
    }
    if (name[0] == ':') {
        if (!invalid_regular
            && namelen == sizeof(":status") - 1
            && memcmp(name, ":status", namelen) == 0) {
            return GRPC_RESPONSE_HEADER_FIELD_STATUS;
        }
        return GRPC_RESPONSE_HEADER_FIELD_REJECTED;
    }
    return invalid_regular
        ? GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR
        : GRPC_RESPONSE_HEADER_FIELD_REGULAR;
}

grpc_response_header_field_route grpc_response_header_route_field(grpc_response_header_block_phase phase, grpc_response_header_field_class field_class)
{
    switch (field_class) {
        case GRPC_RESPONSE_HEADER_FIELD_STATUS:
            return phase == GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS
                ? GRPC_RESPONSE_HEADER_FIELD_ROUTE_STATUS
                : GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR;
        case GRPC_RESPONSE_HEADER_FIELD_REGULAR:
            switch (phase) {
                case GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL:
                    return GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE;
                case GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL:
                case GRPC_RESPONSE_HEADER_BLOCK_TRAILING:
                    return GRPC_RESPONSE_HEADER_FIELD_ROUTE_PROCESS;
                case GRPC_RESPONSE_HEADER_BLOCK_NONE:
                case GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS:
                default:
                    return GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR;
            }
        case GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR:
            switch (phase) {
                case GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL:
                case GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL:
                case GRPC_RESPONSE_HEADER_BLOCK_TRAILING:
                    return GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE;
                case GRPC_RESPONSE_HEADER_BLOCK_NONE:
                case GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS:
                default:
                    return GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR;
            }
        case GRPC_RESPONSE_HEADER_FIELD_REJECTED:
        default:
            /* Rejections which bypass both application field callbacks, and
             * any field class added without an explicit route, fail closed. */
            return GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR;
    }
}

#endif
