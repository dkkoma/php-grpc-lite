#ifndef PHP_GRPC_LITE_TLS_CONFIG_H
#define PHP_GRPC_LITE_TLS_CONFIG_H

#include <stddef.h>
#include <openssl/ssl.h>

int grpc_lite_tls_configure_roots(SSL_CTX *ctx, const char *pem, size_t pem_len);
int grpc_lite_tls_configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len);
const char *grpc_lite_tls_configure_peer_name(SSL *ssl, const char *verify_name);

#endif /* PHP_GRPC_LITE_TLS_CONFIG_H */
