/* Pure gRPC protocol helpers shared by the PHP extension and C unit tests. */

#ifndef PHP_GRPC_LITE_PROTOCOL_CORE_C
#define PHP_GRPC_LITE_PROTOCOL_CORE_C

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen)
{
    int status = 0;

    if (valuelen == 0 || valuelen > 2 || (valuelen > 1 && value[0] == '0')) {
        return -1;
    }

    for (size_t i = 0; i < valuelen; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return -1;
        }
        status = (status * 10) + (value[i] - '0');
    }

    return status <= 16 ? status : -1;
}

static bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen)
{
    static const char prefix[] = "application/grpc";
    size_t prefix_len = sizeof(prefix) - 1;

    if (valuelen < prefix_len || strncasecmp((const char *) value, prefix, prefix_len) != 0) {
        return false;
    }
    if (valuelen == prefix_len) {
        return true;
    }
    return (value[prefix_len] == '+' && valuelen > prefix_len + 1) || value[prefix_len] == ';';
}

static bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen)
{
    return valuelen == sizeof("identity") - 1 && strncasecmp((const char *) value, "identity", sizeof("identity") - 1) == 0;
}

static int grpc_lite_hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static long grpc_lite_ceil_div_timeout(long value, long unit)
{
    return value / unit + (value % unit != 0 ? 1 : 0);
}

static size_t grpc_lite_format_timeout_us(char *buffer, size_t buffer_len, long timeout_us)
{
    long value;
    char unit;
    int written;

    if (buffer == NULL || buffer_len == 0 || timeout_us <= 0) {
        return 0;
    }
    if (timeout_us <= 99999999L) {
        value = timeout_us;
        unit = 'u';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000L);
        unit = 'm';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000000L);
        unit = 'S';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 60000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 60000000L);
        unit = 'M';
    } else {
        value = grpc_lite_ceil_div_timeout(timeout_us, 3600000000L);
        if (value > 99999999L) {
            value = 99999999L;
        }
        unit = 'H';
    }

    written = snprintf(buffer, buffer_len, "%ld%c", value, unit);
    if (written < 0 || (size_t) written >= buffer_len) {
        return 0;
    }
    return (size_t) written;
}

#endif
