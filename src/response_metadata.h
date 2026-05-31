#ifndef PHP_GRPC_LITE_RESPONSE_METADATA_H
#define PHP_GRPC_LITE_RESPONSE_METADATA_H

#include <php.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _grpc_call grpc_call;

int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call);
void grpc_protocol_free_response_metadata_entries(grpc_call *call);
void grpc_protocol_copy_metadata_map(zval *metadata, grpc_call *call, bool trailing);
void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing);

#endif /* PHP_GRPC_LITE_RESPONSE_METADATA_H */
