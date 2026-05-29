#ifndef PHP_GRPC_LITE_SERVER_STREAMING_CALL_H
#define PHP_GRPC_LITE_SERVER_STREAMING_CALL_H

#include "transport.h"

void grpc_lite_streaming_next_result_dtor(grpc_lite_streaming_next_result *result);
int server_streaming_call_open_resource(const char *key, size_t key_len, const char *host, size_t host_len, zend_long port, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_string *primary_user_agent, zend_long timeout_us, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, zend_long max_receive_message_length, size_t max_response_metadata_bytes, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, zval *return_value, grpc_lite_status_result *setup_failure);
int server_streaming_call_next_resource(zval *server_streaming_resource_zv, grpc_lite_streaming_next_result *result);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
int server_streaming_call_next_resource_diagnostic(zval *server_streaming_resource_zv, zval *return_value);
#endif
int server_streaming_call_cancel_resource(zval *server_streaming_resource_zv);

#endif /* PHP_GRPC_LITE_SERVER_STREAMING_CALL_H */
