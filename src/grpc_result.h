#ifndef PHP_GRPC_LITE_GRPC_RESULT_H
#define PHP_GRPC_LITE_GRPC_RESULT_H

#include <php.h>
#include <stdbool.h>

typedef struct {
    int code;
    zend_string *details;
} grpc_lite_status_result;

typedef enum {
    GRPC_LITE_REFUSED_NONE = 0,
    GRPC_LITE_REFUSED_GOAWAY = 1,
    GRPC_LITE_REFUSED_RST_STREAM = 2
} grpc_lite_refused_kind;

typedef struct {
    bool transparent_retryable_unprocessed;
    grpc_lite_refused_kind refused_kind;
    bool response_started;
} grpc_lite_attempt_outcome;

typedef struct {
    zend_string *body;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
    grpc_lite_attempt_outcome outcome;
} grpc_lite_unary_result;

typedef struct {
    bool done;
    zend_string *payload;
    grpc_lite_status_result status;
    zval initial_metadata;
    zval trailing_metadata;
    grpc_lite_attempt_outcome outcome;
} grpc_lite_streaming_next_result;

#endif /* PHP_GRPC_LITE_GRPC_RESULT_H */
