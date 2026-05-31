#include "tls_config.h"

#include <limits.h>
#include <stdbool.h>
#include <openssl/err.h>
#include <openssl/pem.h>

static int add_pem_certs_to_store(X509_STORE *store, const char *pem, size_t pem_len)
{
    if (pem_len > INT_MAX) {
        return -1;
    }
    BIO *bio = BIO_new_mem_buf(pem, (int) pem_len);
    if (bio == NULL) {
        return -1;
    }
    int loaded = 0;
    while (true) {
        X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (cert == NULL) {
            break;
        }
        if (X509_STORE_add_cert(store, cert) == 1) {
            loaded++;
        }
        X509_free(cert);
    }
    BIO_free(bio);
    ERR_clear_error();
    return loaded > 0 ? 0 : -1;
}

int grpc_lite_tls_configure_roots(SSL_CTX *ctx, const char *pem, size_t pem_len)
{
    if (ctx == NULL || pem == NULL || pem_len == 0) {
        return -1;
    }
    return add_pem_certs_to_store(SSL_CTX_get_cert_store(ctx), pem, pem_len);
}

int grpc_lite_tls_configure_client_certificate(SSL_CTX *ctx, const char *cert, size_t cert_len, const char *key, size_t key_len)
{
    if (ctx == NULL || cert == NULL || key == NULL || cert_len > INT_MAX || key_len > INT_MAX) {
        return -1;
    }
    BIO *cert_bio = BIO_new_mem_buf(cert, (int) cert_len);
    BIO *key_bio = BIO_new_mem_buf(key, (int) key_len);
    X509 *x509 = NULL;
    EVP_PKEY *pkey = NULL;
    int ok = 0;
    if (cert_bio == NULL || key_bio == NULL) {
        if (cert_bio != NULL) {
            BIO_free(cert_bio);
        }
        if (key_bio != NULL) {
            BIO_free(key_bio);
        }
        return -1;
    }

    x509 = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    pkey = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(key_bio);
    if (x509 == NULL || pkey == NULL) {
        if (x509 != NULL) {
            X509_free(x509);
        }
        if (pkey != NULL) {
            EVP_PKEY_free(pkey);
        }
        BIO_free(cert_bio);
        return -1;
    }

    ok = SSL_CTX_use_certificate(ctx, x509) == 1
        && SSL_CTX_use_PrivateKey(ctx, pkey) == 1
        && SSL_CTX_check_private_key(ctx) == 1;
    X509_free(x509);
    EVP_PKEY_free(pkey);
    if (!ok) {
        BIO_free(cert_bio);
        return -1;
    }

    while (true) {
        X509 *chain_cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
        if (chain_cert == NULL) {
            break;
        }
        if (SSL_CTX_add_extra_chain_cert(ctx, chain_cert) != 1) {
            X509_free(chain_cert);
            BIO_free(cert_bio);
            return -1;
        }
    }
    BIO_free(cert_bio);
    ERR_clear_error();
    return 0;
}

const char *grpc_lite_tls_configure_peer_name(SSL *ssl, const char *verify_name)
{
    if (ssl == NULL || verify_name == NULL || verify_name[0] == '\0') {
        return "invalid TLS verification host";
    }
    if (SSL_set_tlsext_host_name(ssl, verify_name) != 1) {
        return "failed to configure TLS SNI host";
    }
    if (SSL_set1_host(ssl, verify_name) != 1) {
        return "failed to configure TLS verification host";
    }
    return NULL;
}
