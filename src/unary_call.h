#ifndef PHP_GRPC_LITE_UNARY_CALL_H
#define PHP_GRPC_LITE_UNARY_CALL_H

#include "transport.h"

void grpc_lite_unary_result_dtor(grpc_lite_unary_result *result);
int grpc_lite_unary_call_perform_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, uint64_t deadline_abs_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, uint32_t retry_attempt, grpc_lite_unary_result *result);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
int grpc_lite_unary_call_perform_diagnostic_on_connection(h2_connection *connection, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, zend_long timeout_us, zend_long max_receive_message_length, size_t max_response_metadata_bytes, bool connection_reused, bool persistent_reused, zval *return_value);
#endif

#endif /* PHP_GRPC_LITE_UNARY_CALL_H */
