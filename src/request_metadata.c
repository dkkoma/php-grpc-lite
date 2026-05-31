#include "request_metadata.h"

#include "metadata_key.h"
#include "transport_core.h"

#include <Zend/zend_exceptions.h>
#include <ext/standard/base64.h>
#include <string.h>

static bool is_valid_custom_request_header_name_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '_' || ch == '.' || ch == '-';
}

static bool is_reserved_custom_request_header(const char *name, size_t name_len)
{
    static const char *reserved_headers[] = {
        "content-type",
        "te",
        "grpc-accept-encoding",
        "grpc-encoding",
        "grpc-message",
        "grpc-status",
        "grpc-status-details-bin",
        "host",
        "connection",
        "keep-alive",
        "proxy-connection",
        "transfer-encoding",
        "upgrade",
    };
    size_t index;

    for (index = 0; index < sizeof(reserved_headers) / sizeof(reserved_headers[0]); index++) {
        if (strlen(reserved_headers[index]) == name_len && memcmp(name, reserved_headers[index], name_len) == 0) {
            return true;
        }
    }
    if (name_len > sizeof("grpc-") - 1 && memcmp(name, "grpc-", sizeof("grpc-") - 1) == 0) return true;
    return false;
}

static bool is_forbidden_custom_request_header(zend_string *key)
{
    const char *name;
    size_t name_len;
    size_t index;

    if (key == NULL || ZSTR_LEN(key) == 0) {
        return true;
    }
    name = ZSTR_VAL(key);
    name_len = ZSTR_LEN(key);
    if (name[0] == ':') {
        return true;
    }
    for (index = 0; index < name_len; index++) {
        if (!is_valid_custom_request_header_name_char((unsigned char) name[index])) {
            return true;
        }
    }
    if (is_reserved_custom_request_header(name, name_len)) {
        return true;
    }
    return false;
}

static bool is_user_agent_custom_request_header(zend_string *key)
{
    return key != NULL
        && ZSTR_LEN(key) == sizeof("user-agent") - 1
        && memcmp(ZSTR_VAL(key), "user-agent", sizeof("user-agent") - 1) == 0;
}

static bool is_invalid_binary_request_header_value(zend_string *value)
{
    const char *bytes;
    size_t length;
    size_t index;

    if (value == NULL) {
        return true;
    }
    bytes = ZSTR_VAL(value);
    length = ZSTR_LEN(value);
    for (index = 0; index < length; index++) {
        unsigned char ch = (unsigned char) bytes[index];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=')) {
            return true;
        }
    }
    return false;
}

static bool is_invalid_ascii_request_header_value(zend_string *value)
{
    const char *bytes;
    size_t length;
    size_t index;

    if (value == NULL) {
        return true;
    }
    bytes = ZSTR_VAL(value);
    length = ZSTR_LEN(value);
    for (index = 0; index < length; index++) {
        unsigned char ch = (unsigned char) bytes[index];
        if (ch < 0x20 || ch == 0x7f || ch >= 0x80) {
            return true;
        }
    }
    return false;
}

static int append_custom_request_header_value(h2_request_headers *headers, zend_string *key, zval *value)
{
    zend_string *name_str = NULL;
    zend_string *value_str;

    if (Z_TYPE_P(value) != IS_STRING) {
        zend_throw_exception(NULL, "gRPC request metadata value must be a string", 0);
        return -1;
    }
    if (headers->custom_value_count >= GRPC_LITE_MAX_REQUEST_METADATA_VALUES) {
        zend_throw_exception(NULL, "gRPC request metadata exceeds maximum count", 0);
        return -1;
    }
    name_str = zend_string_copy(key);
    if (grpc_lite_metadata_key_is_binary(name_str)) {
        value_str = php_base64_encode((const unsigned char *) Z_STRVAL_P(value), Z_STRLEN_P(value));
    } else {
        value_str = zend_string_copy(Z_STR_P(value));
    }
    if (grpc_lite_metadata_key_is_binary(name_str) ? is_invalid_binary_request_header_value(value_str) : is_invalid_ascii_request_header_value(value_str)) {
        zend_string_release(name_str);
        zend_string_release(value_str);
        zend_throw_exception(NULL, "invalid gRPC request metadata value", 0);
        return -1;
    }
    if (append_owned_request_header(headers, name_str, value_str) != 0) {
        zend_string_release(name_str);
        zend_string_release(value_str);
        zend_throw_exception(NULL, "gRPC request metadata exceeds maximum count", 0);
        return -1;
    }
    headers->custom_value_count++;
    return 0;
}

int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv)
{
    zend_string *key;
    zval *value;

    if (headers_zv == NULL || Z_TYPE_P(headers_zv) != IS_ARRAY) {
        return 0;
    }

    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
        if (is_user_agent_custom_request_header(key)) {
            continue;
        }
        if (is_forbidden_custom_request_header(key)) {
            zend_throw_exception(NULL, "forbidden gRPC request metadata key", 0);
            return -1;
        }
        if (Z_TYPE_P(value) == IS_ARRAY) {
            zval *nested;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), nested) {
                if (append_custom_request_header_value(headers, key, nested) != 0) {
                    return -1;
                }
            } ZEND_HASH_FOREACH_END();
            continue;
        }

        if (append_custom_request_header_value(headers, key, value) != 0) {
            return -1;
        }
    } ZEND_HASH_FOREACH_END();

    return 0;
}
