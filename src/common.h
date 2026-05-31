#ifndef PHP_GRPC_LITE_COMMON_H
#define PHP_GRPC_LITE_COMMON_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/*
 * Shared private includes and constants for php-grpc-lite.
 * Not installed. Not a public C API.
 *
 * common.h is a transitional aggregate. New narrow headers should include the
 * C/PHP/Zend/nghttp2/OpenSSL headers they actually use, and must not use this
 * file as the default PHP/Zend boundary. Do not add new domain-specific
 * structs, transport policy constants, or diagnostic-only symbols here.
 */

#include "grpc_constants.h"
#include "../php_grpc.h"

#include <php.h>
#include <php_ini.h>
#include <ext/standard/info.h>
#include <ext/standard/base64.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp2/nghttp2.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#endif /* PHP_GRPC_LITE_COMMON_H */
