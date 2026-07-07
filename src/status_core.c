/* gRPC status taxonomy helpers shared by transport and C unit tests. */

#ifndef PHP_GRPC_LITE_STATUS_CORE_C
#define PHP_GRPC_LITE_STATUS_CORE_C

#include "grpc_exchange_state.h"
#include "grpc_constants.h"
#include "status_core.h"

int grpc_lite_status_code_from_call(grpc_call *call, bool cancelled)
{
    if (call->timed_out) return GRPC_STATUS_DEADLINE_EXCEEDED;
    if (cancelled) return GRPC_STATUS_CANCELLED;
    if (call->invalid_grpc_status) return GRPC_STATUS_UNKNOWN;
    if (call->grpc_status < 0 && call->http_status != 200) {
        switch (call->http_status) {
            case 400: return GRPC_STATUS_INTERNAL;
            case 401: return GRPC_STATUS_UNAUTHENTICATED;
            case 403: return GRPC_STATUS_PERMISSION_DENIED;
            case 404: return GRPC_STATUS_UNIMPLEMENTED;
            case 429:
            case 502:
            case 503:
            case 504:
                return GRPC_STATUS_UNAVAILABLE;
            default:
                return call->http_status < 0 ? GRPC_STATUS_UNAVAILABLE : GRPC_STATUS_UNKNOWN;
        }
    }
    if (call->invalid_content_type) return GRPC_STATUS_UNKNOWN;
    if (call->response_message_too_large || call->metadata_too_large || call->response_queue_limit_exceeded) return GRPC_STATUS_RESOURCE_EXHAUSTED;
    if (call->malformed_response_frame) return GRPC_STATUS_INTERNAL;
    if (call->compressed_response_seen || call->unsupported_response_encoding) return GRPC_STATUS_UNIMPLEMENTED;
    if (call->grpc_status >= 0) return call->grpc_status;
    if (call->stream_refused_seen) return GRPC_STATUS_UNAVAILABLE;
    if (call->stream_reset_seen) {
        switch (call->stream_error_code) {
            case NGHTTP2_CANCEL:
                return GRPC_STATUS_CANCELLED;
            case NGHTTP2_REFUSED_STREAM:
                return GRPC_STATUS_UNAVAILABLE;
            case NGHTTP2_ENHANCE_YOUR_CALM:
                return GRPC_STATUS_RESOURCE_EXHAUSTED;
            case NGHTTP2_INADEQUATE_SECURITY:
                return GRPC_STATUS_PERMISSION_DENIED;
            case NGHTTP2_NO_ERROR:
            case NGHTTP2_PROTOCOL_ERROR:
            case NGHTTP2_INTERNAL_ERROR:
            case NGHTTP2_FLOW_CONTROL_ERROR:
            case NGHTTP2_SETTINGS_TIMEOUT:
            case NGHTTP2_STREAM_CLOSED:
            case NGHTTP2_FRAME_SIZE_ERROR:
            case NGHTTP2_COMPRESSION_ERROR:
            case NGHTTP2_CONNECT_ERROR:
                return GRPC_STATUS_INTERNAL;
            default:
                return GRPC_STATUS_UNKNOWN;
        }
    }
    return GRPC_STATUS_UNKNOWN;
}

bool grpc_lite_call_response_started(grpc_call *call)
{
    if (call == NULL) {
        return true;
    }
    if (call->http_status >= 0 || call->metadata_entry_count > 0 || call->grpc_status_seen || call->initial_grpc_status_seen || call->grpc_status >= 0) {
        return true;
    }
    if (call->response_message_count > 0 || call->response_queue_head != NULL) {
        return true;
    }
    if (call->response_header_len != 0 || call->response_payload_len != 0 || call->response_payload_offset != 0 || call->response_payload != NULL) {
        return true;
    }
    if (call->body.s != NULL && ZSTR_LEN(call->body.s) > 0) {
        return true;
    }
    return false;
}

static grpc_lite_refused_kind grpc_lite_refused_kind_from_call(grpc_call *call)
{
    if (call == NULL) {
        return GRPC_LITE_REFUSED_NONE;
    }
    if (call->stream_refused_seen) {
        return GRPC_LITE_REFUSED_GOAWAY;
    }
    if (call->stream_reset_seen && call->stream_error_code == NGHTTP2_REFUSED_STREAM) {
        return GRPC_LITE_REFUSED_RST_STREAM;
    }
    return GRPC_LITE_REFUSED_NONE;
}

void grpc_lite_attempt_outcome_from_call(grpc_call *call, bool userland_response_observed, grpc_lite_attempt_outcome *outcome)
{
    grpc_lite_refused_kind refused_kind;

    if (outcome == NULL) {
        return;
    }
    outcome->transparent_retryable_unprocessed = false;
    outcome->refused_kind = GRPC_LITE_REFUSED_NONE;
    outcome->response_started = grpc_lite_call_response_started(call);

    refused_kind = grpc_lite_refused_kind_from_call(call);
    outcome->refused_kind = refused_kind;
    if (call == NULL || refused_kind == GRPC_LITE_REFUSED_NONE) {
        return;
    }
    outcome->transparent_retryable_unprocessed = call->retry_attempt == 0
        && !outcome->response_started
        && !userland_response_observed;
}

#endif
