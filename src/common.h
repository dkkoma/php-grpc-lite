#ifndef PHP_GRPC_LITE_COMMON_H
#define PHP_GRPC_LITE_COMMON_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/*
 * Shared private includes and constants for php-grpc-lite.
 * Not installed. Not a public C API.
 */

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

#define GRPC_STATUS_OK 0
#define GRPC_STATUS_CANCELLED 1
#define GRPC_STATUS_UNKNOWN 2
#define GRPC_STATUS_INVALID_ARGUMENT 3
#define GRPC_STATUS_DEADLINE_EXCEEDED 4
#define GRPC_STATUS_NOT_FOUND 5
#define GRPC_STATUS_ALREADY_EXISTS 6
#define GRPC_STATUS_PERMISSION_DENIED 7
#define GRPC_STATUS_RESOURCE_EXHAUSTED 8
#define GRPC_STATUS_FAILED_PRECONDITION 9
#define GRPC_STATUS_ABORTED 10
#define GRPC_STATUS_OUT_OF_RANGE 11
#define GRPC_STATUS_UNIMPLEMENTED 12
#define GRPC_STATUS_INTERNAL 13
#define GRPC_STATUS_UNAVAILABLE 14
#define GRPC_STATUS_DATA_LOSS 15
#define GRPC_STATUS_UNAUTHENTICATED 16

#define GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY 16

#define GRPC_OP_SEND_INITIAL_METADATA 0
#define GRPC_OP_SEND_MESSAGE 1
#define GRPC_OP_SEND_CLOSE_FROM_CLIENT 2
#define GRPC_OP_RECV_INITIAL_METADATA 4
#define GRPC_OP_RECV_MESSAGE 5
#define GRPC_OP_RECV_STATUS_ON_CLIENT 6

#endif /* PHP_GRPC_LITE_COMMON_H */
