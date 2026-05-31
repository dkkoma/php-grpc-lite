#ifndef PHP_GRPC_LITE_SURFACE_H
#define PHP_GRPC_LITE_SURFACE_H

#include "module.h"

typedef enum {
    GRPC_LITE_CREDENTIALS_INSECURE = 0,
    GRPC_LITE_CREDENTIALS_SSL = 1,
    GRPC_LITE_CREDENTIALS_DEFAULT = 2
} grpc_lite_credentials_type;

typedef struct {
    grpc_lite_credentials_type type;
    zend_string *root_certs;
    zend_string *private_key;
    zend_string *cert_chain;
    zend_object std;
} grpc_lite_channel_credentials_obj;

typedef struct {
    zval callback;
    zend_object std;
} grpc_lite_call_credentials_obj;

typedef struct {
    zend_string *target;
    zend_string *host;
    zend_long port;
    zend_string *authority;
    zend_string *tls_verify_name;
    zend_string *primary_user_agent;
    zend_string *connection_key;
    zend_long max_receive_message_length;
    size_t max_response_metadata_bytes;
    zval credentials;
    bool initialized;
    zend_object std;
} grpc_lite_channel_obj;

typedef struct {
    zval channel;
    zend_string *method;
    zend_long deadline_us;
    zval credentials;
    zend_string *request_payload;
    zend_string *unary_response_payload;
    zval metadata;
    zval server_streaming_resource;
    zval initial_metadata;
    zval trailing_metadata;
    zval status;
    uint64_t trace_started_us;
    bool unary_performed;
    bool server_streaming_opened;
    bool initial_metadata_ready;
    bool status_ready;
    bool cancelled;
    bool initialized;
    zend_object std;
} grpc_lite_call_obj;

typedef struct {
    zend_long microseconds;
    zend_object std;
} grpc_lite_timeval_obj;

extern zend_class_entry *grpc_ce_channel;
extern zend_class_entry *grpc_ce_call;
extern zend_class_entry *grpc_ce_channel_credentials;
extern zend_class_entry *grpc_ce_call_credentials;
extern zend_class_entry *grpc_ce_timeval;

static inline grpc_lite_channel_credentials_obj *grpc_lite_channel_credentials_fetch(zend_object *obj)
{
    return (grpc_lite_channel_credentials_obj *) ((char *) obj - XtOffsetOf(grpc_lite_channel_credentials_obj, std));
}

static inline grpc_lite_call_credentials_obj *grpc_lite_call_credentials_fetch(zend_object *obj)
{
    return (grpc_lite_call_credentials_obj *) ((char *) obj - XtOffsetOf(grpc_lite_call_credentials_obj, std));
}

static inline grpc_lite_channel_obj *grpc_lite_channel_fetch(zend_object *obj)
{
    return (grpc_lite_channel_obj *) ((char *) obj - XtOffsetOf(grpc_lite_channel_obj, std));
}

static inline grpc_lite_call_obj *grpc_lite_call_fetch(zend_object *obj)
{
    return (grpc_lite_call_obj *) ((char *) obj - XtOffsetOf(grpc_lite_call_obj, std));
}

static inline grpc_lite_timeval_obj *grpc_lite_timeval_fetch(zend_object *obj)
{
    return (grpc_lite_timeval_obj *) ((char *) obj - XtOffsetOf(grpc_lite_timeval_obj, std));
}

#define Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(zv) grpc_lite_channel_credentials_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CALL_CREDENTIALS_P(zv) grpc_lite_call_credentials_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CHANNEL_P(zv) grpc_lite_channel_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_CALL_P(zv) grpc_lite_call_fetch(Z_OBJ_P((zv)))
#define Z_GRPC_LITE_TIMEVAL_P(zv) grpc_lite_timeval_fetch(Z_OBJ_P((zv)))

int grpc_lite_register_surface_classes(void);

#endif /* PHP_GRPC_LITE_SURFACE_H */
