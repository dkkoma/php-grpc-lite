/* Pure response header-block phase transitions shared by all transports. */

#ifndef PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_C
#define PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_C

#include "response_header_phase.h"

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

#endif
