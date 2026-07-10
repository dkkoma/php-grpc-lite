#include <stdio.h>
#include <string.h>

#include "../../src/grpc_exchange_state.h"
#include "../../src/grpc_constants.h"
#include "../../src/status_core.h"

static int failures = 0;

#define ASSERT_STATUS(expected, call_expr, cancelled) do { \
    grpc_call call; \
    memset(&call, 0, sizeof(call)); \
    call.grpc_status = -1; \
    call.http_status = 200; \
    call_expr; \
    int actual_value = grpc_lite_status_code_from_call(&call, (cancelled)); \
    if ((expected) != actual_value) { \
        fprintf(stderr, "%s:%d: expected status %d, got %d\n", __FILE__, __LINE__, (expected), actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_OUTCOME(expected_retryable, expected_kind, expected_started, call_expr, userland_seen) do { \
    grpc_call call; \
    grpc_lite_attempt_outcome outcome; \
    memset(&call, 0, sizeof(call)); \
    memset(&outcome, 0, sizeof(outcome)); \
    call.grpc_status = -1; \
    call.http_status = -1; \
    call_expr; \
    grpc_lite_attempt_outcome_from_call(&call, (userland_seen), &outcome); \
    if ((expected_retryable) != outcome.transparent_retryable_unprocessed) { \
        fprintf(stderr, "%s:%d: expected retryable %d, got %d\n", __FILE__, __LINE__, (expected_retryable), outcome.transparent_retryable_unprocessed); \
        failures++; \
    } \
    if ((expected_kind) != outcome.refused_kind) { \
        fprintf(stderr, "%s:%d: expected refused kind %d, got %d\n", __FILE__, __LINE__, (expected_kind), outcome.refused_kind); \
        failures++; \
    } \
    if ((expected_started) != outcome.response_started) { \
        fprintf(stderr, "%s:%d: expected response_started %d, got %d\n", __FILE__, __LINE__, (expected_started), outcome.response_started); \
        failures++; \
    } \
} while (0)

static void test_priority_order(void)
{
    ASSERT_STATUS(GRPC_STATUS_DEADLINE_EXCEEDED, call.timed_out = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_CANCELLED, call.grpc_status = GRPC_STATUS_OK, true);
    ASSERT_STATUS(GRPC_STATUS_UNKNOWN, call.invalid_grpc_status = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_UNKNOWN, call.invalid_content_type = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_RESOURCE_EXHAUSTED, call.response_message_too_large = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_RESOURCE_EXHAUSTED, call.metadata_too_large = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_RESOURCE_EXHAUSTED, call.response_queue_limit_exceeded = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.malformed_response_frame = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_UNIMPLEMENTED, call.compressed_response_seen = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_UNIMPLEMENTED, call.unsupported_response_encoding = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_STATUS(GRPC_STATUS_PERMISSION_DENIED, call.grpc_status = GRPC_STATUS_PERMISSION_DENIED, false);
}

static void test_http_fallback_mapping(void)
{
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.http_status = 400, false);
    ASSERT_STATUS(GRPC_STATUS_UNAUTHENTICATED, call.http_status = 401, false);
    ASSERT_STATUS(GRPC_STATUS_PERMISSION_DENIED, call.http_status = 403, false);
    ASSERT_STATUS(GRPC_STATUS_UNIMPLEMENTED, call.http_status = 404, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.http_status = 429, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.http_status = 502, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.http_status = 503, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.http_status = 504, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.http_status = -1, false);
    ASSERT_STATUS(GRPC_STATUS_UNKNOWN, call.http_status = 418, false);
}

static void test_http2_stream_error_mapping(void)
{
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.stream_refused_seen = true, false);
    ASSERT_STATUS(GRPC_STATUS_CANCELLED, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_CANCEL, false);
    ASSERT_STATUS(GRPC_STATUS_UNAVAILABLE, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_REFUSED_STREAM, false);
    ASSERT_STATUS(GRPC_STATUS_RESOURCE_EXHAUSTED, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_ENHANCE_YOUR_CALM, false);
    ASSERT_STATUS(GRPC_STATUS_PERMISSION_DENIED, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_INADEQUATE_SECURITY, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_NO_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_PROTOCOL_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_INTERNAL_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_FLOW_CONTROL_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_SETTINGS_TIMEOUT, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_STREAM_CLOSED, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_FRAME_SIZE_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_COMPRESSION_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_INTERNAL, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_CONNECT_ERROR, false);
    ASSERT_STATUS(GRPC_STATUS_UNKNOWN, call.stream_reset_seen = true; call.stream_error_code = 0xffffu, false);
}

static void test_transparent_retryable_unprocessed_predicate(void)
{
    ASSERT_OUTCOME(true, GRPC_LITE_REFUSED_GOAWAY, false, call.stream_refused_seen = true, false);
    ASSERT_OUTCOME(true, GRPC_LITE_REFUSED_RST_STREAM, false, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_REFUSED_STREAM, false);

    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, false, call.retry_attempt = 1; call.stream_refused_seen = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_RST_STREAM, false, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_REFUSED_STREAM, true);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_NONE, false, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_CANCEL, false);

    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.http_status = 200, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.grpc_status_seen = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.grpc_status = GRPC_STATUS_OK, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.metadata_entry_count = 1, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.response_message_count = 1, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, queued_payload payload; memset(&payload, 0, sizeof(payload)); call.stream_refused_seen = true; call.response_queue_head = &payload, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.response_header_len = 1, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.response_payload_len = 1, false);

    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.metadata_too_large = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.content_type_seen = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.invalid_content_type = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.unsupported_response_encoding = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_GOAWAY, true, call.stream_refused_seen = true; call.invalid_grpc_status = true, false);
    ASSERT_OUTCOME(false, GRPC_LITE_REFUSED_RST_STREAM, true, call.stream_reset_seen = true; call.stream_error_code = NGHTTP2_REFUSED_STREAM; call.response_queue_limit_exceeded = true, false);
}

int main(void)
{
    test_priority_order();
    test_http_fallback_mapping();
    test_http2_stream_error_mapping();
    test_transparent_retryable_unprocessed_predicate();

    if (failures != 0) {
        fprintf(stderr, "%d status_core unit assertions failed\n", failures);
        return 1;
    }
    puts("status_core unit tests passed");
    return 0;
}
