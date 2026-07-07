#ifndef PHP_GRPC_LITE_STATUS_CORE_H
#define PHP_GRPC_LITE_STATUS_CORE_H

#include <stdbool.h>

#include "grpc_result.h"

int grpc_lite_status_code_from_call(struct _grpc_call *call, bool cancelled);
bool grpc_lite_call_response_started(struct _grpc_call *call);
void grpc_lite_attempt_outcome_from_call(struct _grpc_call *call, bool userland_response_observed, grpc_lite_attempt_outcome *outcome);

#endif
