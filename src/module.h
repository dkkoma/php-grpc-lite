#ifndef PHP_GRPC_LITE_MODULE_H
#define PHP_GRPC_LITE_MODULE_H

#include "common.h"

ZEND_BEGIN_MODULE_GLOBALS(grpc_lite)
    HashTable persistent_connections;
    bool persistent_connections_initialized;
    zend_string *default_roots_pem;
    zend_long http2_stream_window_size;
    zend_long http2_connection_window_size;
    zend_long http2_max_frame_size;
    zend_long http2_max_header_list_size;
    zend_long server_streaming_read_ahead_max_messages;
    zend_long server_streaming_read_ahead_max_bytes;
ZEND_END_MODULE_GLOBALS(grpc_lite)

ZEND_EXTERN_MODULE_GLOBALS(grpc_lite)

#define PHP_GRPC_LITE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(grpc_lite, v)

#endif /* PHP_GRPC_LITE_MODULE_H */
