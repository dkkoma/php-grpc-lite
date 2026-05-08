#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../protocol_core.c"

static int failures = 0;

#define ASSERT_INT(expected, actual) do { \
    int expected_value = (expected); \
    int actual_value = (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %d, got %d\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_LONG(expected, actual) do { \
    long expected_value = (expected); \
    long actual_value = (actual); \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %ld, got %ld\n", __FILE__, __LINE__, expected_value, actual_value); \
        failures++; \
    } \
} while (0)

#define ASSERT_BOOL(expected, actual) do { \
    int expected_value = (expected) ? 1 : 0; \
    int actual_value = (actual) ? 1 : 0; \
    if (expected_value != actual_value) { \
        fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__, expected_value ? "true" : "false", actual_value ? "true" : "false"); \
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

static void test_grpc_status_parse(void)
{
    ASSERT_INT(0, grpc_protocol_parse_status_value((const uint8_t *) "0", 1));
    ASSERT_INT(9, grpc_protocol_parse_status_value((const uint8_t *) "9", 1));
    ASSERT_INT(16, grpc_protocol_parse_status_value((const uint8_t *) "16", 2));
    ASSERT_INT(-1, grpc_protocol_parse_status_value((const uint8_t *) "", 0));
    ASSERT_INT(-1, grpc_protocol_parse_status_value((const uint8_t *) "01", 2));
    ASSERT_INT(-1, grpc_protocol_parse_status_value((const uint8_t *) "17", 2));
    ASSERT_INT(-1, grpc_protocol_parse_status_value((const uint8_t *) "1x", 2));
    ASSERT_INT(-1, grpc_protocol_parse_status_value((const uint8_t *) "100", 3));
}

static void test_content_type(void)
{
    ASSERT_BOOL(true, grpc_protocol_is_valid_content_type((const uint8_t *) "application/grpc", strlen("application/grpc")));
    ASSERT_BOOL(true, grpc_protocol_is_valid_content_type((const uint8_t *) "Application/Grpc", strlen("Application/Grpc")));
    ASSERT_BOOL(true, grpc_protocol_is_valid_content_type((const uint8_t *) "application/grpc+proto", strlen("application/grpc+proto")));
    ASSERT_BOOL(true, grpc_protocol_is_valid_content_type((const uint8_t *) "application/grpc; charset=utf-8", strlen("application/grpc; charset=utf-8")));
    ASSERT_BOOL(false, grpc_protocol_is_valid_content_type((const uint8_t *) "application/grpcfoo", strlen("application/grpcfoo")));
    ASSERT_BOOL(false, grpc_protocol_is_valid_content_type((const uint8_t *) "application/grpc+", strlen("application/grpc+")));
    ASSERT_BOOL(false, grpc_protocol_is_valid_content_type((const uint8_t *) "application/json", strlen("application/json")));
}

static void test_identity_encoding(void)
{
    ASSERT_BOOL(true, grpc_protocol_is_identity_encoding((const uint8_t *) "identity", strlen("identity")));
    ASSERT_BOOL(true, grpc_protocol_is_identity_encoding((const uint8_t *) "Identity", strlen("Identity")));
    ASSERT_BOOL(false, grpc_protocol_is_identity_encoding((const uint8_t *) "gzip", strlen("gzip")));
    ASSERT_BOOL(false, grpc_protocol_is_identity_encoding((const uint8_t *) "identity ", strlen("identity ")));
}

static void test_hex_value(void)
{
    ASSERT_INT(0, grpc_lite_hex_value('0'));
    ASSERT_INT(9, grpc_lite_hex_value('9'));
    ASSERT_INT(10, grpc_lite_hex_value('A'));
    ASSERT_INT(15, grpc_lite_hex_value('F'));
    ASSERT_INT(10, grpc_lite_hex_value('a'));
    ASSERT_INT(15, grpc_lite_hex_value('f'));
    ASSERT_INT(-1, grpc_lite_hex_value('g'));
    ASSERT_INT(-1, grpc_lite_hex_value('%'));
}

static void test_timeout_format(void)
{
    char buffer[32];

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(0, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 0));

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(2, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 1));
    ASSERT_STR("1u", buffer);

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(9, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 99999999L));
    ASSERT_STR("99999999u", buffer);

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(7, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 100000000L));
    ASSERT_STR("100000m", buffer);

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(7, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 99999999001L));
    ASSERT_STR("100000S", buffer);

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(9, (long) grpc_lite_format_timeout_us(buffer, sizeof(buffer), 600000000000000000L));
    ASSERT_STR("99999999H", buffer);

    memset(buffer, 0, sizeof(buffer));
    ASSERT_LONG(0, (long) grpc_lite_format_timeout_us(buffer, 2, 12345L));
}

int main(void)
{
    test_grpc_status_parse();
    test_content_type();
    test_identity_encoding();
    test_hex_value();
    test_timeout_format();

    if (failures != 0) {
        fprintf(stderr, "%d protocol_core unit assertions failed\n", failures);
        return 1;
    }
    puts("protocol_core unit tests passed");
    return 0;
}
