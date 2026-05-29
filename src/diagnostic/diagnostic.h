#ifndef PHP_GRPC_LITE_DIAGNOSTIC_H
#define PHP_GRPC_LITE_DIAGNOSTIC_H

#include "../server_streaming_call.h"
#include "../unary_call.h"

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
void grpc_lite_diagnostic_add_unary_result(zval *diagnostic_result, const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status, uint64_t start_unix_nanos, uint64_t total_us, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused);
void grpc_lite_diagnostic_add_server_streaming_status(zval *diagnostic_result, server_streaming_call_state *state, grpc_lite_status_result *status);
#endif

#endif /* PHP_GRPC_LITE_DIAGNOSTIC_H */
