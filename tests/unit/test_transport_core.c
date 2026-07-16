#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "../../src/transport_core.h"

static int failures = 0;

#define ASSERT_BOOL(expected, actual) do { \
    int expected_value = (expected) ? 1 : 0; \
    int actual_value = (actual) ? 1 : 0; \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__, expected_value ? "true" : "false", actual_value ? "true" : "false"); \
        failures++; \
    } \
} while (0)

#define ASSERT_SIZE(expected, actual) do { \
    size_t expected_value = (size_t) (expected); \
    size_t actual_value = (size_t) (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %zu, got %zu\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_UINT32(expected, actual) do { \
    uint32_t expected_value = (uint32_t) (expected); \
    uint32_t actual_value = (uint32_t) (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %" PRIu32 ", got %" PRIu32 "\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_STR(expected, actual) do { \
    const char *expected_value = (expected); \
    const char *actual_value = (actual); \
    if (strcmp(expected_value, actual_value) != 0) { \
        fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_NULL(actual) do { \
    const char *actual_value = (actual); \
    if (actual_value != NULL) { \
        fprintf(stderr, "%s:%d: expected NULL, got %s\n", __FILE__, __LINE__, actual_value); \
        failures++; \
    } \
} while (0)

static void test_effective_limits(void)
{
    ASSERT_SIZE(SIZE_MAX, effective_max_receive_message_bytes(-1));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES, effective_max_receive_message_bytes(0));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_MAX_RECEIVE_MESSAGE_BYTES, effective_max_receive_message_bytes(-2));
    ASSERT_SIZE(123, effective_max_receive_message_bytes(123));

    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE, effective_http2_window_size(0));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE, effective_http2_window_size(GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE - 1));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE, effective_http2_window_size(GRPC_LITE_HTTP2_DEFAULT_WINDOW_SIZE));
    ASSERT_UINT32(8 * 1024 * 1024, effective_http2_window_size(8 * 1024 * 1024));
    ASSERT_UINT32(GRPC_LITE_HTTP2_MAX_WINDOW_SIZE, effective_http2_window_size(GRPC_LITE_HTTP2_MAX_WINDOW_SIZE + 1L));
    ASSERT_UINT32(GRPC_LITE_HTTP2_MAX_WINDOW_SIZE, effective_http2_window_size(INT64_MAX));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(0));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE - 1));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE));
    ASSERT_UINT32(65536, effective_http2_max_frame_size(65536));
    ASSERT_UINT32(GRPC_LITE_HTTP2_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_MAX_FRAME_SIZE + 1L));
    ASSERT_UINT32(GRPC_LITE_HTTP2_MAX_FRAME_SIZE, effective_http2_max_frame_size(INT64_MAX));
    ASSERT_UINT32(0, effective_http2_max_header_list_size(-1));
    ASSERT_UINT32(0, effective_http2_max_header_list_size(0));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE, effective_http2_max_header_list_size(GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE));
    ASSERT_UINT32(UINT32_MAX, effective_http2_max_header_list_size((int64_t) UINT32_MAX + 1));
    ASSERT_UINT32(UINT32_MAX, effective_http2_max_header_list_size(INT64_MAX));

    ASSERT_SIZE(4096, effective_max_response_metadata_bytes(1024, 4096));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_METADATA_HARD_BYTES, effective_max_response_metadata_bytes(1024, -1));
    ASSERT_SIZE(20480, effective_max_response_metadata_bytes(16384, -1));
    ASSERT_SIZE((uint64_t) INT64_MAX > (uint64_t) SIZE_MAX ? SIZE_MAX : (size_t) INT64_MAX, effective_max_response_metadata_bytes(1024, INT64_MAX));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES, effective_max_response_metadata_bytes(-1, -1));

    /* default mfs (16384): 4 * (16384 + 9) = 65572 — a full DATA chunk
     * (9 + mfs) must fit so it does not bypass coalescing */
    ASSERT_SIZE(65572, h2_write_coalesce_capacity_for_max_frame_size(16384));
    ASSERT_BOOL(true, h2_write_coalesce_capacity_for_max_frame_size(16384) >= 16384 + 9);
    /* tiny mfs clamps up to the minimum capacity */
    ASSERT_SIZE(GRPC_LITE_H2_WRITE_COALESCE_MIN_CAPACITY, h2_write_coalesce_capacity_for_max_frame_size(0));
    /* mfs = 64KB still coalesces: 4 * (65536 + 9) = 262180 */
    ASSERT_SIZE(262180, h2_write_coalesce_capacity_for_max_frame_size(65536));
    /* above ~256KB the capacity clamps to the 1MB ceiling (frames bypass) */
    ASSERT_SIZE(GRPC_LITE_H2_WRITE_COALESCE_MAX_CAPACITY, h2_write_coalesce_capacity_for_max_frame_size(262136));
    ASSERT_SIZE(GRPC_LITE_H2_WRITE_COALESCE_MAX_CAPACITY, h2_write_coalesce_capacity_for_max_frame_size(GRPC_LITE_HTTP2_MAX_FRAME_SIZE));
}

static void test_response_header_budget(void)
{
    size_t entry_count = 0;
    size_t bytes = 0;

    for (size_t i = 0; i < GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES; i++) {
        ASSERT_BOOL(true, grpc_response_header_budget_account_field(&entry_count, &bytes, SIZE_MAX, 1, 1));
    }
    ASSERT_SIZE(GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES, entry_count);
    ASSERT_SIZE(GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES * 2, bytes);
    ASSERT_BOOL(false, grpc_response_header_budget_account_field(&entry_count, &bytes, SIZE_MAX, 1, 1));
    ASSERT_SIZE(GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES, entry_count);
    ASSERT_SIZE(GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES * 2, bytes);

    entry_count = 0;
    bytes = 0;
    ASSERT_BOOL(true, grpc_response_header_budget_account_field(&entry_count, &bytes, 8, 3, 5));
    ASSERT_SIZE(1, entry_count);
    ASSERT_SIZE(8, bytes);
    ASSERT_BOOL(false, grpc_response_header_budget_account_field(&entry_count, &bytes, 8, 1, 0));
    ASSERT_SIZE(1, entry_count);
    ASSERT_SIZE(8, bytes);

    entry_count = 0;
    bytes = 0;
    ASSERT_BOOL(false, grpc_response_header_budget_account_field(&entry_count, &bytes, SIZE_MAX, SIZE_MAX, 1));
    ASSERT_SIZE(0, entry_count);
    ASSERT_SIZE(0, bytes);

    entry_count = 1;
    bytes = SIZE_MAX;
    ASSERT_BOOL(false, grpc_response_header_budget_account_field(&entry_count, &bytes, SIZE_MAX, 1, 0));
    ASSERT_SIZE(1, entry_count);
    ASSERT_SIZE(SIZE_MAX, bytes);
}

static size_t append_test_frame(uint8_t *buffer, uint32_t payload_len, uint8_t type, uint8_t flags, uint8_t payload_byte)
{
    buffer[0] = (uint8_t) ((payload_len >> 16) & 0xff);
    buffer[1] = (uint8_t) ((payload_len >> 8) & 0xff);
    buffer[2] = (uint8_t) (payload_len & 0xff);
    buffer[3] = type;
    buffer[4] = flags;
    buffer[5] = 0;
    buffer[6] = 0;
    buffer[7] = 0;
    buffer[8] = 1;
    memset(buffer + GRPC_LITE_HTTP2_FRAME_HEADER_SIZE, payload_byte, payload_len);
    return GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + payload_len;
}

static void test_receive_boundary_reuse_predicate(void)
{
    grpc_h2_receive_boundary_state state;

    grpc_h2_receive_boundary_state_reset(&state);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    state.frame_header_bytes = 1;
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    state.frame_header_bytes = 0;
    state.frame_payload_remaining = 1;
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    state.frame_payload_remaining = 0;
    state.header_block_in_flight = true;
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    state.frame_header_bytes = 1;
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(NULL));
}

static void test_receive_boundary_partial_headers_payload(void)
{
    grpc_h2_receive_boundary_state state;
    uint8_t frame[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 3];
    size_t frame_len = append_test_frame(frame, 3, 0x1, 0x4, 0x82);

    for (size_t header_bytes = 1; header_bytes < GRPC_LITE_HTTP2_FRAME_HEADER_SIZE; header_bytes++) {
        grpc_h2_receive_boundary_state_reset(&state);
        grpc_h2_receive_boundary_state_consume(&state, frame, header_bytes);
        ASSERT_SIZE(header_bytes, state.frame_header_bytes);
        ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    }

    grpc_h2_receive_boundary_state_reset(&state);
    grpc_h2_receive_boundary_state_consume(&state, frame, GRPC_LITE_HTTP2_FRAME_HEADER_SIZE);
    ASSERT_SIZE(0, state.frame_header_bytes);
    ASSERT_UINT32(3, state.frame_payload_remaining);
    ASSERT_UINT32(0x1, state.frame_type);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_consume(&state, frame + GRPC_LITE_HTTP2_FRAME_HEADER_SIZE, 1);
    ASSERT_UINT32(2, state.frame_payload_remaining);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_consume(&state, frame + GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 1, 2);
    ASSERT_UINT32(0, state.frame_payload_remaining);
    ASSERT_BOOL(false, state.header_block_in_flight);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    ASSERT_SIZE(frame_len, sizeof(frame));
}

static void test_receive_boundary_fragmented_header_block(void)
{
    grpc_h2_receive_boundary_state state;
    uint8_t headers[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 1];
    uint8_t continuation[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE];
    uint8_t final_continuation[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 2];
    size_t headers_len = append_test_frame(headers, 1, 0x1, 0, 0x82);
    size_t continuation_len = append_test_frame(continuation, 0, 0x9, 0, 0);
    size_t final_len = append_test_frame(final_continuation, 2, 0x9, 0x4, 0x84);

    grpc_h2_receive_boundary_state_reset(&state);
    grpc_h2_receive_boundary_state_consume(&state, headers, headers_len);
    ASSERT_SIZE(0, state.frame_header_bytes);
    ASSERT_UINT32(0, state.frame_payload_remaining);
    ASSERT_BOOL(true, state.header_block_in_flight);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_consume(&state, continuation, continuation_len);
    ASSERT_BOOL(true, state.header_block_in_flight);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_consume(
        &state,
        final_continuation,
        GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 1
    );
    ASSERT_UINT32(1, state.frame_payload_remaining);
    ASSERT_UINT32(0x9, state.frame_type);
    ASSERT_BOOL(true, state.header_block_in_flight);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_consume(
        &state,
        final_continuation + GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 1,
        final_len - GRPC_LITE_HTTP2_FRAME_HEADER_SIZE - 1
    );
    ASSERT_BOOL(false, state.header_block_in_flight);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));
}

static void test_receive_boundary_partial_data_and_zero_length_frames(void)
{
    grpc_h2_receive_boundary_state state;
    uint8_t data_frame[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 4];
    uint8_t empty_headers[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE];
    uint8_t empty_continuation[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE];
    uint8_t empty_data[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE];
    size_t data_len = append_test_frame(data_frame, 4, 0x0, 0, 0x42);

    grpc_h2_receive_boundary_state_reset(&state);
    grpc_h2_receive_boundary_state_consume(&state, data_frame, GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 2);
    ASSERT_UINT32(2, state.frame_payload_remaining);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    grpc_h2_receive_boundary_state_consume(
        &state,
        data_frame + GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 2,
        data_len - GRPC_LITE_HTTP2_FRAME_HEADER_SIZE - 2
    );
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    append_test_frame(empty_headers, 0, 0x1, 0x4, 0);
    grpc_h2_receive_boundary_state_consume(&state, empty_headers, sizeof(empty_headers));
    ASSERT_BOOL(false, state.header_block_in_flight);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    append_test_frame(empty_headers, 0, 0x1, 0, 0);
    grpc_h2_receive_boundary_state_consume(&state, empty_headers, sizeof(empty_headers));
    ASSERT_BOOL(true, state.header_block_in_flight);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    append_test_frame(empty_continuation, 0, 0x9, 0x4, 0);
    grpc_h2_receive_boundary_state_consume(&state, empty_continuation, sizeof(empty_continuation));
    ASSERT_BOOL(false, state.header_block_in_flight);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    append_test_frame(empty_data, 0, 0x0, 0, 0);
    grpc_h2_receive_boundary_state_consume(&state, empty_data, sizeof(empty_data));
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));
}

static void test_receive_boundary_multiple_frames_and_chunk_splits(void)
{
    grpc_h2_receive_boundary_state state;
    uint8_t wire[(GRPC_LITE_HTTP2_FRAME_HEADER_SIZE * 3) + 5];
    uint8_t next_frame[GRPC_LITE_HTTP2_FRAME_HEADER_SIZE + 1];
    size_t wire_len = 0;
    size_t next_len;
    size_t offset;
    static const size_t chunks[] = {1, 2, 7, 3, 5, 4, 9, 6};

    wire_len += append_test_frame(wire + wire_len, 0, 0x4, 0, 0);
    wire_len += append_test_frame(wire + wire_len, 2, 0x1, 0x4, 0x82);
    wire_len += append_test_frame(wire + wire_len, 3, 0x0, 0, 0x42);
    ASSERT_SIZE(sizeof(wire), wire_len);

    grpc_h2_receive_boundary_state_reset(&state);
    grpc_h2_receive_boundary_state_consume(&state, wire, wire_len);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    grpc_h2_receive_boundary_state_reset(&state);
    offset = 0;
    for (size_t index = 0; offset < wire_len; index++) {
        size_t chunk = chunks[index % (sizeof(chunks) / sizeof(chunks[0]))];
        if (chunk > wire_len - offset) {
            chunk = wire_len - offset;
        }
        grpc_h2_receive_boundary_state_consume(&state, wire + offset, chunk);
        offset += chunk;
    }
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));

    next_len = append_test_frame(next_frame, 1, 0x0, 0, 0x24);
    grpc_h2_receive_boundary_state_consume(&state, next_frame, 4);
    ASSERT_SIZE(4, state.frame_header_bytes);
    ASSERT_BOOL(false, grpc_h2_receive_allows_reuse_after_abandonment(&state));
    grpc_h2_receive_boundary_state_consume(&state, next_frame + 4, next_len - 4);
    ASSERT_BOOL(true, grpc_h2_receive_allows_reuse_after_abandonment(&state));
}

static void test_authority_identity(void)
{
    char authority[32];

    memset(authority, 0, sizeof(authority));
    build_authority(authority, sizeof(authority), "example.test", 443, NULL, 0);
    ASSERT_STR("example.test:443", authority);

    memset(authority, 0, sizeof(authority));
    build_authority(authority, sizeof(authority), "example.test", 65535, NULL, 0);
    ASSERT_STR("example.test:65535", authority);

    memset(authority, 0, sizeof(authority));
    build_authority(authority, sizeof(authority), "example.test", 443, "override.test", strlen("override.test"));
    ASSERT_STR("override.test", authority);

}

static void test_grpc_path_validation(void)
{
    ASSERT_NULL(validate_grpc_path("/package.Service/Method", strlen("/package.Service/Method")));
    ASSERT_NULL(validate_grpc_path("/S/M", strlen("/S/M")));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path(NULL, 0));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path("", 0));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path("package.Service/Method", strlen("package.Service/Method")));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path("/Service", strlen("/Service")));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path("/Service/Method With Space", strlen("/Service/Method With Space")));
    ASSERT_STR("invalid gRPC method path", validate_grpc_path("/Service/\x7f", strlen("/Service/\x7f")));
}

static void test_channel_input_validation(void)
{
    char long_key[514];
    char long_host[GRPC_LITE_AUTHORITY_BUFFER_SIZE];
    memset(long_key, 'k', sizeof(long_key));
    memset(long_host, 'h', sizeof(long_host));

    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, "authority.test", strlen("authority.test"), "verify.test", strlen("verify.test")));

    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs("", 0, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs(long_key, 513, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs("bad\nkey", strlen("bad\nkey"), "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target host", validate_channel_inputs("key", 3, "", 0, 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target host", validate_channel_inputs("key", 3, "bad\177host", strlen("bad\177host"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), -1, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 0, NULL, 0, NULL, 0));
    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 1, NULL, 0, NULL, 0));
    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 65535, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 65536, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), INT64_MAX, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC authority", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, "user@authority", strlen("user@authority"), NULL, 0));
    ASSERT_STR("invalid gRPC authority", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, "authority/path", strlen("authority/path"), NULL, 0));
    ASSERT_STR("gRPC authority is too long", validate_channel_inputs("key", 3, long_host, sizeof(long_host), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid TLS verify name", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, NULL, 0, "bad/verify", strlen("bad/verify")));
}

static void test_control_character_detection(void)
{
    ASSERT_BOOL(false, contains_nul_or_control(NULL, 0));
    ASSERT_BOOL(false, contains_authority_forbidden_char(NULL, 0));
    ASSERT_BOOL(false, contains_nul_or_control("authority", strlen("authority")));
    ASSERT_BOOL(false, contains_authority_forbidden_char("authority.test:443", strlen("authority.test:443")));
    ASSERT_BOOL(true, contains_nul_or_control("bad\nvalue", strlen("bad\nvalue")));
    ASSERT_BOOL(true, contains_authority_forbidden_char("bad\\authority", strlen("bad\\authority")));
}

int main(void)
{
    test_effective_limits();
    test_response_header_budget();
    test_receive_boundary_reuse_predicate();
    test_receive_boundary_partial_headers_payload();
    test_receive_boundary_fragmented_header_block();
    test_receive_boundary_partial_data_and_zero_length_frames();
    test_receive_boundary_multiple_frames_and_chunk_splits();
    test_authority_identity();
    test_grpc_path_validation();
    test_channel_input_validation();
    test_control_character_detection();

    if (failures != 0) {
        fprintf(stderr, "%d transport_core unit assertions failed\n", failures);
        return 1;
    }
    puts("transport_core unit tests passed");
    return 0;
}
