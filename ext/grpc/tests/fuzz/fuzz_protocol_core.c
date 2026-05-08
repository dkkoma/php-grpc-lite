#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../protocol_core.c"

static long fuzz_timeout_us(const uint8_t *data, size_t size)
{
    unsigned long value = 0;
    size_t copy_len = size < sizeof(value) ? size : sizeof(value);

    if (copy_len > 0) {
        memcpy(&value, data, copy_len);
    }
    return (long) (value & (unsigned long) LONG_MAX);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t selector;
    char timeout_buffer[16];
    size_t timeout_buffer_len;

    if (size == 0) {
        (void) grpc_protocol_parse_status_value(data, size);
        (void) grpc_protocol_is_valid_content_type(data, size);
        (void) grpc_lite_format_timeout_us(timeout_buffer, sizeof(timeout_buffer), 0);
        return 0;
    }

    selector = data[0] % 6;
    data++;
    size--;

    switch (selector) {
        case 0:
            (void) grpc_protocol_parse_status_value(data, size);
            break;
        case 1:
            (void) grpc_protocol_is_valid_content_type(data, size);
            break;
        case 2:
            (void) grpc_protocol_is_identity_encoding(data, size);
            break;
        case 3:
            if (size > 0) {
                (void) grpc_lite_hex_value(data[0]);
            }
            break;
        case 4:
            timeout_buffer_len = size > 0 ? (size_t) (data[0] % sizeof(timeout_buffer)) : 0;
            (void) grpc_lite_format_timeout_us(timeout_buffer, timeout_buffer_len, fuzz_timeout_us(data, size));
            break;
        default:
            (void) grpc_protocol_parse_status_value(data, size);
            (void) grpc_protocol_is_valid_content_type(data, size);
            (void) grpc_protocol_is_identity_encoding(data, size);
            if (size > 0) {
                (void) grpc_lite_hex_value(data[0]);
            }
            (void) grpc_lite_format_timeout_us(timeout_buffer, sizeof(timeout_buffer), fuzz_timeout_us(data, size));
            break;
    }

    return 0;
}
