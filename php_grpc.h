#ifndef PHP_GRPC_H
#define PHP_GRPC_H

#include "php.h"

#define PHP_GRPC_VERSION "0.0.13"
#define PHP_GRPC_LITE_USER_AGENT "php-grpc-lite/" PHP_GRPC_VERSION
#define PHP_GRPC_LITE_BENCH_USER_AGENT PHP_GRPC_LITE_USER_AGENT "-dev"

extern zend_module_entry grpc_module_entry;
#define phpext_grpc_ptr &grpc_module_entry

extern const zend_function_entry grpc_lite_functions[];

#endif /* PHP_GRPC_H */
