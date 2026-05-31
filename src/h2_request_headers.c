#include "h2_request_headers.h"

#include "protocol_core.h"
#include "transport_core.h"
#include "../php_grpc.h"

#include <string.h>

int init_request_headers(h2_request_headers *headers)
{
    headers->capacity = GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY;
    headers->len = 0;
    headers->name_count = 0;
    headers->value_count = 0;
    headers->custom_value_count = 0;
    headers->nva = headers->inline_nva;
    headers->name_strings = headers->inline_name_strings;
    headers->value_strings = headers->inline_value_strings;
    return 0;
}

static void grow_request_headers(h2_request_headers *headers)
{
    size_t new_capacity = headers->capacity * 2;
    nghttp2_nv *new_nva;
    zend_string **new_name_strings;
    zend_string **new_value_strings;

    if (new_capacity < headers->capacity || new_capacity > GRPC_LITE_MAX_REQUEST_METADATA_VALUES + 7) {
        new_capacity = GRPC_LITE_MAX_REQUEST_METADATA_VALUES + 7;
    }
    if (new_capacity <= headers->capacity) {
        return;
    }

    new_nva = ecalloc(new_capacity, sizeof(nghttp2_nv));
    new_name_strings = ecalloc(new_capacity, sizeof(zend_string *));
    new_value_strings = ecalloc(new_capacity, sizeof(zend_string *));

    memcpy(new_nva, headers->nva, headers->len * sizeof(nghttp2_nv));
    memcpy(new_name_strings, headers->name_strings, headers->name_count * sizeof(zend_string *));
    memcpy(new_value_strings, headers->value_strings, headers->value_count * sizeof(zend_string *));

    if (headers->nva != headers->inline_nva) {
        efree(headers->nva);
    }
    if (headers->name_strings != headers->inline_name_strings) {
        efree(headers->name_strings);
    }
    if (headers->value_strings != headers->inline_value_strings) {
        efree(headers->value_strings);
    }

    headers->nva = new_nva;
    headers->name_strings = new_name_strings;
    headers->value_strings = new_value_strings;
    headers->capacity = new_capacity;
}

static int ensure_request_header_capacity(h2_request_headers *headers)
{
    if (headers->len >= headers->capacity || headers->name_count >= headers->capacity || headers->value_count >= headers->capacity) {
        grow_request_headers(headers);
    }
    if (headers->name_strings == NULL || headers->value_strings == NULL || headers->len >= headers->capacity || headers->name_count >= headers->capacity || headers->value_count >= headers->capacity) {
        return -1;
    }
    return 0;
}

void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen)
{
    if (headers->len >= headers->capacity) {
        grow_request_headers(headers);
        if (headers->len >= headers->capacity) {
            return;
        }
    }
    headers->nva[headers->len++] = (nghttp2_nv) {
        (uint8_t *) name,
        (uint8_t *) value,
        namelen,
        valuelen,
        NGHTTP2_NV_FLAG_NONE
    };
}

int append_owned_request_header(h2_request_headers *headers, zend_string *name, zend_string *value)
{
    if (ensure_request_header_capacity(headers) != 0) {
        return -1;
    }
    headers->name_strings[headers->name_count++] = name;
    headers->value_strings[headers->value_count++] = value;
    append_request_header(headers, ZSTR_VAL(name), ZSTR_LEN(name), ZSTR_VAL(value), ZSTR_LEN(value));
    return 0;
}

void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us)
{
    char timeout_buf[32];
    size_t timeout_len;
    zend_string *value_str;
    timeout_len = grpc_lite_format_timeout_us(timeout_buf, sizeof(timeout_buf), (long) timeout_us);
    if (timeout_len == 0) {
        return;
    }
    value_str = zend_string_init(timeout_buf, timeout_len, 0);
    if (headers->value_strings != NULL && headers->value_count < headers->capacity) {
        headers->value_strings[headers->value_count++] = value_str;
    } else {
        zend_string_release(value_str);
        return;
    }
    append_request_header(headers, "grpc-timeout", sizeof("grpc-timeout") - 1, ZSTR_VAL(value_str), ZSTR_LEN(value_str));
}

void append_user_agent_request_header(h2_request_headers *headers, zend_string *primary_user_agent)
{
    if (primary_user_agent != NULL && ZSTR_LEN(primary_user_agent) > 0) {
        append_request_header(headers, "user-agent", sizeof("user-agent") - 1, ZSTR_VAL(primary_user_agent), ZSTR_LEN(primary_user_agent));
        return;
    }
    append_request_header(headers, "user-agent", sizeof("user-agent") - 1, PHP_GRPC_LITE_USER_AGENT, sizeof(PHP_GRPC_LITE_USER_AGENT) - 1);
}

void free_request_headers(h2_request_headers *headers)
{
    size_t i;
    if (headers->name_strings != NULL) {
        for (i = 0; i < headers->name_count; i++) {
            if (headers->name_strings[i] != NULL) {
                zend_string_release(headers->name_strings[i]);
            }
        }
        if (headers->name_strings != headers->inline_name_strings) {
            efree(headers->name_strings);
        }
    }
    if (headers->value_strings != NULL) {
        for (i = 0; i < headers->value_count; i++) {
            if (headers->value_strings[i] != NULL) {
                zend_string_release(headers->value_strings[i]);
            }
        }
        if (headers->value_strings != headers->inline_value_strings) {
            efree(headers->value_strings);
        }
    }
    if (headers->nva != NULL && headers->nva != headers->inline_nva) {
        efree(headers->nva);
    }
    headers->nva = NULL;
    headers->name_strings = NULL;
    headers->value_strings = NULL;
    headers->len = 0;
    headers->capacity = 0;
    headers->name_count = 0;
    headers->value_count = 0;
    headers->custom_value_count = 0;
}
