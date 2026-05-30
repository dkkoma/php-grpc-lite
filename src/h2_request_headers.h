#ifndef PHP_GRPC_LITE_H2_REQUEST_HEADERS_H
#define PHP_GRPC_LITE_H2_REQUEST_HEADERS_H

#include <nghttp2/nghttp2.h>
#include <php.h>
#include <stddef.h>

#define GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY 16

typedef struct _grpc_call grpc_call;

typedef struct {
    nghttp2_nv *nva;
    size_t len;
    size_t capacity;
    zend_string **name_strings;
    size_t name_count;
    zend_string **value_strings;
    size_t value_count;
    size_t custom_value_count;
    nghttp2_nv inline_nva[GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY];
    zend_string *inline_name_strings[GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY];
    zend_string *inline_value_strings[GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY];
} h2_request_headers;

void grpc_lite_trace_request_headers(grpc_call *call, const nghttp2_nv *headers, size_t header_count);
int init_request_headers(h2_request_headers *headers);
void append_request_header(h2_request_headers *headers, const char *name, size_t namelen, const char *value, size_t valuelen);
void append_grpc_timeout_request_header(h2_request_headers *headers, zend_long timeout_us);
void append_user_agent_request_header(h2_request_headers *headers, zend_string *primary_user_agent);
int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);
void free_request_headers(h2_request_headers *headers);

#endif /* PHP_GRPC_LITE_H2_REQUEST_HEADERS_H */
