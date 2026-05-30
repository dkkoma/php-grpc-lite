#ifndef PHP_GRPC_LITE_GRPC_RESULT_H
#define PHP_GRPC_LITE_GRPC_RESULT_H

#include "common.h"

typedef struct {
    int code;
    zend_string *details;
} grpc_lite_status_result;

typedef struct {
    zend_string *body;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
} grpc_lite_unary_result;

typedef struct {
    bool done;
    zend_string *payload;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
} grpc_lite_streaming_next_result;

#endif /* PHP_GRPC_LITE_GRPC_RESULT_H */
