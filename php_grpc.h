#ifndef PHP_GRPC_H
#define PHP_GRPC_H

#include "php.h"

#define PHP_GRPC_VERSION "0.1.0"

extern zend_module_entry grpc_module_entry;
#define phpext_grpc_ptr &grpc_module_entry

#endif /* PHP_GRPC_H */
