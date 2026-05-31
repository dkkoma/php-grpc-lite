#ifndef PHP_GRPC_LITE_METADATA_KEY_H
#define PHP_GRPC_LITE_METADATA_KEY_H

#include <php.h>
#include <stdbool.h>
#include <string.h>

static inline bool grpc_lite_metadata_key_is_binary(zend_string *key)
{
    return key != NULL
        && ZSTR_LEN(key) >= sizeof("-bin") - 1
        && memcmp(ZSTR_VAL(key) + ZSTR_LEN(key) - (sizeof("-bin") - 1), "-bin", sizeof("-bin") - 1) == 0;
}

#endif /* PHP_GRPC_LITE_METADATA_KEY_H */
