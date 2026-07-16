#ifndef PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_H
#define PHP_GRPC_LITE_RESPONSE_HEADER_PHASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GRPC_RESPONSE_HEADER_BLOCK_NONE = 0,
    GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS,
    GRPC_RESPONSE_HEADER_BLOCK_INFORMATIONAL,
    GRPC_RESPONSE_HEADER_BLOCK_FINAL_INITIAL,
    GRPC_RESPONSE_HEADER_BLOCK_TRAILING,
} grpc_response_header_block_phase;

typedef struct {
    grpc_response_header_block_phase block_phase;
    bool final_response_headers_seen;
    bool trailers_only_candidate;
} grpc_response_header_phase_state;

typedef enum {
    GRPC_RESPONSE_HEADER_FIELD_REJECTED = 0,
    GRPC_RESPONSE_HEADER_FIELD_STATUS,
    GRPC_RESPONSE_HEADER_FIELD_REGULAR,
    GRPC_RESPONSE_HEADER_FIELD_INVALID_REGULAR,
} grpc_response_header_field_class;

typedef enum {
    GRPC_RESPONSE_HEADER_FIELD_ROUTE_TERMINAL_PROTOCOL_ERROR = 0,
    GRPC_RESPONSE_HEADER_FIELD_ROUTE_STATUS,
    GRPC_RESPONSE_HEADER_FIELD_ROUTE_PROCESS,
    GRPC_RESPONSE_HEADER_FIELD_ROUTE_IGNORE,
} grpc_response_header_field_route;

void grpc_response_header_phase_reset(grpc_response_header_phase_state *state);
grpc_response_header_block_phase grpc_response_header_phase_begin(grpc_response_header_phase_state *state);
grpc_response_header_block_phase grpc_response_header_phase_on_status(grpc_response_header_phase_state *state, int http_status);
grpc_response_header_block_phase grpc_response_header_phase_end(grpc_response_header_phase_state *state);
bool grpc_response_header_phase_allows_status_fields(const grpc_response_header_phase_state *state, bool end_stream);
bool grpc_response_header_phase_on_trailers_only_status_field(grpc_response_header_phase_state *state, bool end_stream);
bool grpc_response_header_phase_metadata_is_trailing(const grpc_response_header_phase_state *state);
grpc_response_header_field_class grpc_response_header_classify_reported_field(const uint8_t *name, size_t namelen, bool invalid_regular);
grpc_response_header_field_route grpc_response_header_route_field(grpc_response_header_block_phase phase, grpc_response_header_field_class field_class);

#endif
