#ifndef PHP_GRPC_LITE_STATUS_CORE_H
#define PHP_GRPC_LITE_STATUS_CORE_H

#include <stdbool.h>

int grpc_lite_status_code_from_call(struct _grpc_call *call, bool cancelled);

#endif
