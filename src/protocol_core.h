#ifndef PHP_GRPC_LITE_PROTOCOL_CORE_H
#define PHP_GRPC_LITE_PROTOCOL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int grpc_protocol_parse_status_value(const uint8_t *value, size_t valuelen);
bool grpc_protocol_is_valid_content_type(const uint8_t *value, size_t valuelen);
bool grpc_protocol_is_identity_encoding(const uint8_t *value, size_t valuelen);
int grpc_lite_hex_value(unsigned char ch);
size_t grpc_lite_format_timeout_us(char *buffer, size_t buffer_len, long timeout_us);

#endif
