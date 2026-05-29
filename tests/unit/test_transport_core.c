#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "../../src/transport.h"
#ifdef snprintf
#undef snprintf
#endif
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
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(0));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE - 1));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE));
    ASSERT_UINT32(65536, effective_http2_max_frame_size(65536));
    ASSERT_UINT32(GRPC_LITE_HTTP2_MAX_FRAME_SIZE, effective_http2_max_frame_size(GRPC_LITE_HTTP2_MAX_FRAME_SIZE + 1L));
    ASSERT_UINT32(0, effective_http2_max_header_list_size(-1));
    ASSERT_UINT32(0, effective_http2_max_header_list_size(0));
    ASSERT_UINT32(GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE, effective_http2_max_header_list_size(GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE));
    ASSERT_UINT32(UINT32_MAX, effective_http2_max_header_list_size((zend_long) UINT32_MAX + 1L));

    ASSERT_SIZE(4096, effective_max_response_metadata_bytes(1024, 4096));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_METADATA_HARD_BYTES, effective_max_response_metadata_bytes(1024, -1));
    ASSERT_SIZE(20480, effective_max_response_metadata_bytes(16384, -1));
    ASSERT_SIZE(GRPC_LITE_DEFAULT_RESPONSE_METADATA_BYTES, effective_max_response_metadata_bytes(-1, -1));
}

static void test_authority_identity(void)
{
    char authority[32];

    memset(authority, 0, sizeof(authority));
    build_authority(authority, sizeof(authority), "example.test", 443, NULL, 0);
    ASSERT_STR("example.test:443", authority);

    memset(authority, 0, sizeof(authority));
    build_authority(authority, sizeof(authority), "example.test", 443, "override.test", strlen("override.test"));
    ASSERT_STR("override.test", authority);

    ASSERT_SIZE(0, hash_bytes(NULL, 0));
    ASSERT_SIZE(0, hash_bytes("", 0));
    ASSERT_BOOL(true, hash_bytes("authority-a", strlen("authority-a")) == hash_bytes("authority-a", strlen("authority-a")));
    ASSERT_BOOL(true, hash_bytes("authority-a", strlen("authority-a")) != hash_bytes("authority-b", strlen("authority-b")));
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
    char long_host[sizeof(((h2_connection *) 0)->authority)];
    memset(long_key, 'k', sizeof(long_key));
    memset(long_host, 'h', sizeof(long_host));

    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_NULL(validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 50051, "authority.test", strlen("authority.test"), "verify.test", strlen("verify.test")));

    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs("", 0, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs(long_key, 513, "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid grpc_lite connection key", validate_channel_inputs("bad\nkey", strlen("bad\nkey"), "test-server", strlen("test-server"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target host", validate_channel_inputs("key", 3, "", 0, 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target host", validate_channel_inputs("key", 3, "bad\177host", strlen("bad\177host"), 50051, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 0, NULL, 0, NULL, 0));
    ASSERT_STR("invalid gRPC target port", validate_channel_inputs("key", 3, "test-server", strlen("test-server"), 65536, NULL, 0, NULL, 0));
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
