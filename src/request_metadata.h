#ifndef PHP_GRPC_LITE_REQUEST_METADATA_H
#define PHP_GRPC_LITE_REQUEST_METADATA_H

#include "h2_request_headers.h"

int append_custom_request_headers(h2_request_headers *headers, zval *headers_zv);

#endif /* PHP_GRPC_LITE_REQUEST_METADATA_H */
