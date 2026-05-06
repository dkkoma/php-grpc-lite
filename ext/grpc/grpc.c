#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "grpc_internal.h"

ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)

static int le_h2_stream;
static zend_class_entry *grpc_ce_channel;
static zend_class_entry *grpc_ce_call;
static zend_class_entry *grpc_ce_channel_credentials;
static zend_class_entry *grpc_ce_call_credentials;
static zend_class_entry *grpc_ce_timeval;
static zend_object_handlers grpc_channel_handlers;
static zend_object_handlers grpc_call_handlers;
static zend_object_handlers grpc_channel_credentials_handlers;
static zend_object_handlers grpc_call_credentials_handlers;
static zend_object_handlers grpc_timeval_handlers;
static zend_string *grpc_default_roots_pem;

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

#define GRPC_OP_SEND_INITIAL_METADATA 0
#define GRPC_OP_SEND_MESSAGE 1
#define GRPC_OP_SEND_CLOSE_FROM_CLIENT 2
#define GRPC_OP_RECV_INITIAL_METADATA 4
#define GRPC_OP_RECV_MESSAGE 5
#define GRPC_OP_RECV_STATUS_ON_CLIENT 6

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
    zend_long max_receive_message_length;
    zval credentials;
    zend_object std;
} grpc_lite_channel_obj;

typedef struct {
    zval channel;
    zend_string *method;
    zend_long deadline_us;
    zval credentials;
    zend_string *request;
    zval metadata;
    zval stream;
    zval initial_metadata;
    zval trailing_metadata;
    zval status;
    bool sent;
    bool unary_performed;
    bool stream_opened;
    bool initial_metadata_ready;
    bool status_ready;
    bool cancelled;
    zend_object std;
} grpc_lite_call_obj;

typedef struct {
    zend_long microseconds;
    zend_object std;
} grpc_lite_timeval_obj;

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

#include "grpc_transport.c"

static void terminate_stream_with_cancel(h2_stream *stream)
{
    if (stream == NULL) {
        return;
    }
    free_queued_response_payloads(&stream->client);
    if (channel_owned_by_stream(stream->channel, stream) && channel_usable(stream->channel) && stream->client.stream_id > 0) {
        int rv = nghttp2_submit_rst_stream(stream->channel->session, NGHTTP2_FLAG_NONE, stream->client.stream_id, NGHTTP2_CANCEL);
        if (rv == 0) {
            rv = nghttp2_session_send(stream->channel->session);
        }
        if (rv != 0) {
            mark_channel_dead(stream->channel, rv);
            detach_persistent_channel_by_ptr(stream->channel);
        }
    }
    stream->completed = true;
}

static int perform_h2_channel_unary(h2_channel *channel, const char *path, size_t path_len, const char *request, size_t request_len, zval *headers_zv, zend_long timeout_us, zend_long max_receive_message_length, bool channel_reused, bool persistent_reused, zval *return_value)
{
    grpc_call client;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    int rv;
    char recv_buf[16384];
    uint64_t total_started = 0;
    uint64_t setup_started = 0;
    uint64_t setup_us = 0;
    uint64_t submit_started = 0;
    uint64_t submit_us = 0;
    uint64_t initial_send_started = 0;
    uint64_t initial_send_us = 0;
    uint64_t recv_loop_started = 0;
    uint64_t recv_loop_us = 0;
    if (!channel_usable(channel)) {
        zend_throw_exception(NULL, "invalid grpc_lite channel", 0);
        return FAILURE;
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "HTTP/2 channel already has an active stream", 0);
        return FAILURE;
    }
    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }

    memset(&client, 0, sizeof(client));
    client.fd = channel->fd;
    client.channel = channel;
    client.grpc_status = -1;
    client.http_status = -1;
    client.max_response_messages = 1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    client.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    total_started = monotonic_us();
    client.deadline_abs_us = timeout_us > 0 ? total_started + (uint64_t) timeout_us : 0;
    if (set_socket_timeout_us(channel->fd, timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        return FAILURE;
    }

    setup_started = monotonic_us();
    // cppcheck-suppress autoVariables
    nghttp2_session_set_user_data(channel->session, &client);
    channel->busy = true;
    // cppcheck-suppress autoVariables
    channel->active_call_owner = &client;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        clear_channel_call_owner(channel, &client);
        cleanup_grpc_call(&client);
        return FAILURE;
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, channel->tls ? "https" : "http", channel->tls ? sizeof("https") - 1 : sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, channel->authority, strlen(channel->authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        return FAILURE;
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    setup_us = monotonic_us() - setup_started;

    submit_started = monotonic_us();
    client.stream_id = nghttp2_submit_request(channel->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    submit_us = monotonic_us() - submit_started;
    if (client.stream_id < 0) {
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        return FAILURE;
    }

    initial_send_started = monotonic_us();
    rv = nghttp2_session_send(channel->session);
    initial_send_us = monotonic_us() - initial_send_started;
    if (rv != 0) {
        mark_channel_dead(channel, rv);
        if (client.timed_out) {
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            recv_loop_us = 0;
            goto build_unary_result;
        }
        clear_channel_call_owner(channel, &client);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        return FAILURE;
    }

    recv_loop_started = monotonic_us();
    while (!client.stream_closed) {
        ssize_t nread = channel_recv(channel, (uint8_t *) recv_buf, sizeof(recv_buf), client.deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && client.deadline_abs_us > 0;
            if (nread < 0) {
                client.last_io_errno = errno;
                client.last_ssl_error = channel->last_ssl_error;
                snprintf(client.last_io_error_detail, sizeof(client.last_io_error_detail), "%s", channel->last_error_detail);
            }
            if (socket_timeout) {
                client.timed_out = true;
            }
            if (!client.stream_closed) {
                mark_channel_dead(channel, nread == 0 ? 0 : errno);
            }
            break;
        }
        client.bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(channel->session, (const uint8_t *) recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_channel_dead(channel, rv);
            clear_channel_call_owner(channel, &client);
            free_request_headers(&request_headers);
            cleanup_grpc_call(&client);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            return FAILURE;
        }
        if (client.metadata_too_large) {
            rv = nghttp2_session_send(channel->session);
            if (rv != 0) {
                mark_channel_dead(channel, rv);
            }
            if (!client.stream_closed) {
                mark_channel_dead(channel, NGHTTP2_CANCEL);
            }
            break;
        }
        rv = nghttp2_session_send(channel->session);
        if (rv != 0) {
            mark_channel_dead(channel, rv);
            break;
        }
        client.last_session_error = rv;
    }
    recv_loop_us = monotonic_us() - recv_loop_started;
build_unary_result:
    clear_channel_call_owner(channel, &client);

    array_init(return_value);
    smart_str_0(&client.body);
    add_assoc_str(return_value, "body", client.body.s ? zend_string_copy(client.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_str(return_value, "grpc_message", client.grpc_message != NULL ? zend_string_copy(client.grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
    add_assoc_bool(return_value, "stream_reset_seen", client.stream_reset_seen);
    add_assoc_bool(return_value, "invalid_grpc_status", client.invalid_grpc_status);
    add_assoc_str(return_value, "content_type", client.content_type != NULL ? zend_string_copy(client.content_type) : zend_empty_string);
    add_assoc_str(return_value, "grpc_encoding", client.grpc_encoding != NULL ? zend_string_copy(client.grpc_encoding) : zend_empty_string);
    add_assoc_bool(return_value, "compressed_response_seen", client.compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", client.response_message_too_large);
    add_assoc_bool(return_value, "malformed_response_frame", client.malformed_response_frame);
    add_assoc_bool(return_value, "metadata_too_large", client.metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", client.invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", client.unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", client.max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) client.max_receive_message_bytes);
    add_assoc_long(return_value, "body_bytes", client.body.s ? ZSTR_LEN(client.body.s) : 0);
    add_assoc_long(return_value, "request_offset", client.request_offset);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "data_read_calls", client.data_read_calls);
    add_assoc_long(return_value, "data_recv_calls", client.data_recv_calls);
    add_assoc_long(return_value, "last_session_error", client.last_session_error);
    add_assoc_long(return_value, "last_sent_frame_type", client.last_sent_frame_type);
    add_assoc_long(return_value, "last_recv_frame_type", client.last_recv_frame_type);
    add_assoc_long(return_value, "last_sent_frame_flags", client.last_sent_frame_flags);
    add_assoc_long(return_value, "last_recv_frame_flags", client.last_recv_frame_flags);
    add_assoc_long(return_value, "last_not_sent_frame_type", client.last_not_sent_frame_type);
    add_assoc_long(return_value, "last_not_sent_error", client.last_not_sent_error);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_long(return_value, "not_sent_frames", client.not_sent_frames);
    add_assoc_long(return_value, "last_io_errno", client.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", client.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", client.last_io_error_detail);
    add_assoc_long(return_value, "total_us", (zend_long) (monotonic_us() - total_started));
    add_assoc_long(return_value, "connect_us", 0);
    add_assoc_long(return_value, "setup_us", (zend_long) setup_us);
    add_assoc_long(return_value, "submit_us", (zend_long) submit_us);
    add_assoc_long(return_value, "initial_send_us", (zend_long) initial_send_us);
    add_assoc_long(return_value, "recv_loop_us", (zend_long) recv_loop_us);
    add_assoc_long(return_value, "cleanup_us", 0);
    add_assoc_bool(return_value, "timed_out", client.timed_out);
    add_assoc_bool(return_value, "channel_reused", channel_reused);
    add_assoc_bool(return_value, "persistent_reused", persistent_reused);
    add_assoc_bool(return_value, "channel_dead", channel->dead);
    add_assoc_bool(return_value, "channel_draining", channel->draining);
    add_assoc_long(return_value, "channel_last_error", channel->last_error);
    add_assoc_long(return_value, "channel_last_io_errno", channel->last_io_errno);
    add_assoc_long(return_value, "channel_last_ssl_error", channel->last_ssl_error);
    add_assoc_long(return_value, "channel_tls_verify_result", (zend_long) channel->tls_verify_result);
    add_assoc_string(return_value, "channel_last_error_detail", channel->last_error_detail);
    add_assoc_string(return_value, "channel_negotiated_protocol", channel->negotiated_protocol);
    add_assoc_long(return_value, "channel_last_goaway_error_code", channel->last_goaway_error_code);
    add_assoc_long(return_value, "channel_last_goaway_stream_id", channel->last_goaway_stream_id);
    add_metadata_map_to_return(return_value, "initial_metadata", &client, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", &client, true);
    free_request_headers(&request_headers);
    cleanup_grpc_call(&client);
    return SUCCESS;
}

static zend_object *grpc_lite_channel_credentials_create_object(zend_class_entry *ce)
{
    grpc_lite_channel_credentials_obj *obj = zend_object_alloc(sizeof(grpc_lite_channel_credentials_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &grpc_channel_credentials_handlers;
    return &obj->std;
}

static void grpc_lite_channel_credentials_free_object(zend_object *object)
{
    grpc_lite_channel_credentials_obj *obj = grpc_lite_channel_credentials_fetch(object);
    if (obj->root_certs != NULL) {
        zend_string_release(obj->root_certs);
    }
    if (obj->private_key != NULL) {
        zend_string_release(obj->private_key);
    }
    if (obj->cert_chain != NULL) {
        zend_string_release(obj->cert_chain);
    }
    zend_object_std_dtor(&obj->std);
}

static void grpc_lite_channel_credentials_init(zval *return_value, grpc_lite_credentials_type type, zend_string *root_certs, zend_string *private_key, zend_string *cert_chain)
{
    grpc_lite_channel_credentials_obj *obj;
    object_init_ex(return_value, grpc_ce_channel_credentials);
    obj = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(return_value);
    obj->type = type;
    if (root_certs != NULL) {
        obj->root_certs = zend_string_copy(root_certs);
    } else if ((type == GRPC_LITE_CREDENTIALS_SSL || type == GRPC_LITE_CREDENTIALS_DEFAULT) && grpc_default_roots_pem != NULL) {
        obj->root_certs = zend_string_copy(grpc_default_roots_pem);
    }
    if (private_key != NULL) {
        obj->private_key = zend_string_copy(private_key);
    }
    if (cert_chain != NULL) {
        obj->cert_chain = zend_string_copy(cert_chain);
    }
}

PHP_METHOD(ChannelCredentials, createInsecure)
{
    ZEND_PARSE_PARAMETERS_NONE();
    grpc_lite_channel_credentials_init(return_value, GRPC_LITE_CREDENTIALS_INSECURE, NULL, NULL, NULL);
}

PHP_METHOD(ChannelCredentials, createSsl)
{
    zend_string *root_certs = NULL;
    zend_string *private_key = NULL;
    zend_string *cert_chain = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(root_certs)
        Z_PARAM_STR_OR_NULL(private_key)
        Z_PARAM_STR_OR_NULL(cert_chain)
    ZEND_PARSE_PARAMETERS_END();

    grpc_lite_channel_credentials_init(return_value, GRPC_LITE_CREDENTIALS_SSL, root_certs, private_key, cert_chain);
}

PHP_METHOD(ChannelCredentials, createDefault)
{
    ZEND_PARSE_PARAMETERS_NONE();
    grpc_lite_channel_credentials_init(return_value, GRPC_LITE_CREDENTIALS_DEFAULT, NULL, NULL, NULL);
}

PHP_METHOD(ChannelCredentials, setDefaultRootsPem)
{
    zend_string *roots;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(roots)
    ZEND_PARSE_PARAMETERS_END();

    if (grpc_default_roots_pem != NULL) {
        zend_string_release(grpc_default_roots_pem);
    }
    grpc_default_roots_pem = zend_string_copy(roots);
}

PHP_METHOD(ChannelCredentials, isDefaultRootsPemSet)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(grpc_default_roots_pem != NULL);
}

PHP_METHOD(ChannelCredentials, invalidateDefaultRootsPem)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (grpc_default_roots_pem != NULL) {
        zend_string_release(grpc_default_roots_pem);
        grpc_default_roots_pem = NULL;
    }
}

static zend_object *grpc_lite_call_credentials_create_object(zend_class_entry *ce)
{
    grpc_lite_call_credentials_obj *obj = zend_object_alloc(sizeof(grpc_lite_call_credentials_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    ZVAL_UNDEF(&obj->callback);
    obj->std.handlers = &grpc_call_credentials_handlers;
    return &obj->std;
}

static void grpc_lite_call_credentials_free_object(zend_object *object)
{
    grpc_lite_call_credentials_obj *obj = grpc_lite_call_credentials_fetch(object);
    zval_ptr_dtor(&obj->callback);
    zend_object_std_dtor(&obj->std);
}

PHP_METHOD(CallCredentials, createFromPlugin)
{
    zval *callback;
    grpc_lite_call_credentials_obj *obj;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, NULL)) {
        zend_throw_exception(NULL, "CallCredentials plugin must be callable", 0);
        RETURN_THROWS();
    }
    object_init_ex(return_value, grpc_ce_call_credentials);
    obj = Z_GRPC_LITE_CALL_CREDENTIALS_P(return_value);
    ZVAL_COPY(&obj->callback, callback);
}

static zend_object *grpc_lite_timeval_create_object(zend_class_entry *ce)
{
    grpc_lite_timeval_obj *obj = zend_object_alloc(sizeof(grpc_lite_timeval_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &grpc_timeval_handlers;
    return &obj->std;
}

PHP_METHOD(Timeval, __construct)
{
    zend_long microseconds;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(microseconds)
    ZEND_PARSE_PARAMETERS_END();
    Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds = microseconds;
}

PHP_METHOD(Timeval, now)
{
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = (zend_long) monotonic_us();
}

PHP_METHOD(Timeval, infFuture)
{
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = ZEND_LONG_MAX;
}

PHP_METHOD(Timeval, infPast)
{
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = ZEND_LONG_MIN;
}

PHP_METHOD(Timeval, add)
{
    zval *other;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(other, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds + Z_GRPC_LITE_TIMEVAL_P(other)->microseconds;
}

PHP_METHOD(Timeval, subtract)
{
    zval *other;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(other, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds - Z_GRPC_LITE_TIMEVAL_P(other)->microseconds;
}

PHP_METHOD(Timeval, compare)
{
    zval *other;
    zend_long value;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(other, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    value = Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds - Z_GRPC_LITE_TIMEVAL_P(other)->microseconds;
    RETURN_LONG(value < 0 ? -1 : (value > 0 ? 1 : 0));
}

PHP_METHOD(Timeval, microtime)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds);
}

PHP_METHOD(Timeval, sleepUntil)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_TRUE;
}

PHP_METHOD(Timeval, zero)
{
    ZEND_PARSE_PARAMETERS_NONE();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = 0;
}

static zend_object *grpc_lite_channel_create_object(zend_class_entry *ce)
{
    grpc_lite_channel_obj *obj = zend_object_alloc(sizeof(grpc_lite_channel_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    ZVAL_UNDEF(&obj->credentials);
    obj->port = 443;
    obj->max_receive_message_length = 0;
    obj->std.handlers = &grpc_channel_handlers;
    return &obj->std;
}

static void grpc_lite_channel_free_object(zend_object *object)
{
    grpc_lite_channel_obj *obj = grpc_lite_channel_fetch(object);
    if (obj->target != NULL) zend_string_release(obj->target);
    if (obj->host != NULL) zend_string_release(obj->host);
    if (obj->authority != NULL) zend_string_release(obj->authority);
    if (obj->tls_verify_name != NULL) zend_string_release(obj->tls_verify_name);
    if (obj->primary_user_agent != NULL) zend_string_release(obj->primary_user_agent);
    zval_ptr_dtor(&obj->credentials);
    zend_object_std_dtor(&obj->std);
}

static int split_target_to_host_port(zend_string *target, zend_string **host, zend_long *port)
{
    const char *value = ZSTR_VAL(target);
    size_t len = ZSTR_LEN(target);
    const char *colon = memrchr(value, ':', len);
    if (colon == NULL || colon == value || colon == value + len - 1) {
        *host = zend_string_copy(target);
        *port = 443;
        return SUCCESS;
    }
    *host = zend_string_init(value, (size_t) (colon - value), 0);
    *port = zend_atol(colon + 1, (size_t) (value + len - colon - 1));
    return SUCCESS;
}

PHP_METHOD(Channel, __construct)
{
    zend_string *target;
    zval *opts;
    zval *credentials;
    zval *authority;
    zval *tls_verify_name;
    zval *user_agent;
    zval *max_receive_message_length;
    grpc_lite_channel_obj *obj = Z_GRPC_LITE_CHANNEL_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(target)
        Z_PARAM_ARRAY(opts)
    ZEND_PARSE_PARAMETERS_END();

    credentials = zend_hash_str_find(Z_ARRVAL_P(opts), "credentials", sizeof("credentials") - 1);
    if (credentials == NULL || Z_TYPE_P(credentials) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(credentials), grpc_ce_channel_credentials)) {
        zend_throw_exception(NULL, "Channel options must include a ChannelCredentials instance", 0);
        RETURN_THROWS();
    }
    obj->target = zend_string_copy(target);
    split_target_to_host_port(target, &obj->host, &obj->port);
    ZVAL_COPY(&obj->credentials, credentials);

    authority = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.default_authority", sizeof("grpc.default_authority") - 1);
    if (authority != NULL && Z_TYPE_P(authority) == IS_STRING) {
        obj->authority = zend_string_copy(Z_STR_P(authority));
    }
    tls_verify_name = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.ssl_target_name_override", sizeof("grpc.ssl_target_name_override") - 1);
    if (tls_verify_name != NULL && Z_TYPE_P(tls_verify_name) == IS_STRING) {
        obj->tls_verify_name = zend_string_copy(Z_STR_P(tls_verify_name));
    }
    user_agent = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.primary_user_agent", sizeof("grpc.primary_user_agent") - 1);
    if (user_agent != NULL && Z_TYPE_P(user_agent) == IS_STRING) {
        obj->primary_user_agent = zend_string_copy(Z_STR_P(user_agent));
    }
    max_receive_message_length = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.max_receive_message_length", sizeof("grpc.max_receive_message_length") - 1);
    if (max_receive_message_length != NULL) {
        if (Z_TYPE_P(max_receive_message_length) != IS_LONG || Z_LVAL_P(max_receive_message_length) < -1) {
            zend_throw_exception(NULL, "grpc.max_receive_message_length must be -1 or non-negative", 0);
            RETURN_THROWS();
        }
        obj->max_receive_message_length = Z_LVAL_P(max_receive_message_length);
    }
}

PHP_METHOD(Channel, getTarget)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STR_COPY(Z_GRPC_LITE_CHANNEL_P(ZEND_THIS)->target);
}

PHP_METHOD(Channel, close)
{
    grpc_lite_channel_obj *obj = Z_GRPC_LITE_CHANNEL_P(ZEND_THIS);
    zend_string *key;
    key = strpprintf(0, "%s|%ld|%s|%s|%d",
        ZSTR_VAL(obj->host),
        obj->port,
        obj->authority != NULL ? ZSTR_VAL(obj->authority) : "",
        obj->tls_verify_name != NULL ? ZSTR_VAL(obj->tls_verify_name) : "",
        Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&obj->credentials)->type);
    if (PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        h2_channel *channel = zend_hash_find_ptr(&PHP_GRPC_LITE_G(persistent_channels), key);
        if (channel != NULL) {
            discard_persistent_channel(ZSTR_VAL(key), ZSTR_LEN(key), channel);
        }
    }
    zend_string_release(key);
}

PHP_METHOD(Channel, getConnectivityState)
{
    bool try_to_connect = false;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(try_to_connect)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_LONG(2);
}

PHP_METHOD(Channel, watchConnectivityState)
{
    zend_long state;
    zval *deadline;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(state)
        Z_PARAM_OBJECT_OF_CLASS(deadline, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    RETURN_FALSE;
}

static zend_object *grpc_lite_call_create_object(zend_class_entry *ce)
{
    grpc_lite_call_obj *obj = zend_object_alloc(sizeof(grpc_lite_call_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    ZVAL_UNDEF(&obj->channel);
    ZVAL_UNDEF(&obj->credentials);
    ZVAL_UNDEF(&obj->stream);
    array_init(&obj->metadata);
    array_init(&obj->initial_metadata);
    array_init(&obj->trailing_metadata);
    ZVAL_UNDEF(&obj->status);
    obj->std.handlers = &grpc_call_handlers;
    return &obj->std;
}

static void grpc_lite_call_free_object(zend_object *object)
{
    grpc_lite_call_obj *obj = grpc_lite_call_fetch(object);
    zval_ptr_dtor(&obj->channel);
    zval_ptr_dtor(&obj->credentials);
    zval_ptr_dtor(&obj->stream);
    zval_ptr_dtor(&obj->metadata);
    zval_ptr_dtor(&obj->initial_metadata);
    zval_ptr_dtor(&obj->trailing_metadata);
    zval_ptr_dtor(&obj->status);
    if (obj->method != NULL) zend_string_release(obj->method);
    if (obj->request != NULL) zend_string_release(obj->request);
    zend_object_std_dtor(&obj->std);
}

PHP_METHOD(Call, __construct)
{
    zval *channel;
    zend_string *method;
    zval *deadline;
    grpc_lite_call_obj *obj = Z_GRPC_LITE_CALL_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(channel, grpc_ce_channel)
        Z_PARAM_STR(method)
        Z_PARAM_OBJECT_OF_CLASS(deadline, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();

    ZVAL_COPY(&obj->channel, channel);
    obj->method = zend_string_copy(method);
    obj->deadline_us = Z_GRPC_LITE_TIMEVAL_P(deadline)->microseconds;
}

PHP_METHOD(Call, setCredentials)
{
    zval *credentials;
    grpc_lite_call_obj *obj = Z_GRPC_LITE_CALL_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(credentials, grpc_ce_call_credentials)
    ZEND_PARSE_PARAMETERS_END();

    zval_ptr_dtor(&obj->credentials);
    ZVAL_COPY(&obj->credentials, credentials);
}

static zend_long grpc_lite_call_timeout_us(grpc_lite_call_obj *call)
{
    uint64_t now;
    if (call->deadline_us <= 0 || call->deadline_us == ZEND_LONG_MAX) {
        return 0;
    }
    now = monotonic_us();
    if ((uint64_t) call->deadline_us <= now) {
        return 1;
    }
    if ((uint64_t) call->deadline_us - now > (uint64_t) ZEND_LONG_MAX) {
        return ZEND_LONG_MAX;
    }
    return (zend_long) ((uint64_t) call->deadline_us - now);
}

static void grpc_lite_copy_metadata(zval *dest, zval *src)
{
    zval_ptr_dtor(dest);
    array_init(dest);
    if (src == NULL || Z_TYPE_P(src) != IS_ARRAY) {
        return;
    }
    zend_hash_copy(Z_ARRVAL_P(dest), Z_ARRVAL_P(src), zval_add_ref);
}

static void grpc_lite_append_timeout_metadata(zval *metadata, zend_long timeout_us)
{
    char timeout_buf[32];
    zval timeout_values;
    if (timeout_us <= 0) {
        return;
    }
    snprintf(timeout_buf, sizeof(timeout_buf), "%ldu", timeout_us);
    array_init(&timeout_values);
    add_next_index_string(&timeout_values, timeout_buf);
    zend_hash_str_update(Z_ARRVAL_P(metadata), "grpc-timeout", sizeof("grpc-timeout") - 1, &timeout_values);
}

static void grpc_lite_append_user_agent(grpc_lite_channel_obj *channel, zval *metadata)
{
    zval values;
    array_init(&values);
    if (channel->primary_user_agent != NULL && ZSTR_LEN(channel->primary_user_agent) > 0) {
        add_next_index_str(&values, zend_string_copy(channel->primary_user_agent));
    } else {
        add_next_index_string(&values, "php-grpc-lite/0.1.0");
    }
    zend_hash_str_update(Z_ARRVAL_P(metadata), "user-agent", sizeof("user-agent") - 1, &values);
}

static int grpc_lite_merge_call_credentials_metadata(grpc_lite_call_obj *call, grpc_lite_channel_obj *channel)
{
    grpc_lite_call_credentials_obj *credentials;
    zval function_name;
    zval params[2];
    zval retval;
    zend_string *service_url;
    const char *method = ZSTR_VAL(call->method);
    size_t method_len = ZSTR_LEN(call->method);
    const char *last_slash = memrchr(method, '/', method_len);
    size_t service_len = last_slash != NULL ? (size_t) (last_slash - method) : method_len;

    if (Z_TYPE(call->credentials) != IS_OBJECT || !instanceof_function(Z_OBJCE(call->credentials), grpc_ce_call_credentials)) {
        return SUCCESS;
    }
    credentials = Z_GRPC_LITE_CALL_CREDENTIALS_P(&call->credentials);
    if (Z_TYPE(credentials->callback) == IS_UNDEF) {
        return SUCCESS;
    }

    service_url = strpprintf(0, "%s://%s%.*s",
        Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials)->type == GRPC_LITE_CREDENTIALS_INSECURE ? "http" : "https",
        ZSTR_VAL(channel->target),
        (int) service_len,
        method);
    ZVAL_COPY(&function_name, &credentials->callback);
    ZVAL_STR_COPY(&params[0], service_url);
    ZVAL_STR_COPY(&params[1], call->method);
    ZVAL_UNDEF(&retval);
    if (call_user_function(EG(function_table), NULL, &function_name, &retval, 2, params) != SUCCESS || EG(exception)) {
        zval_ptr_dtor(&function_name);
        zval_ptr_dtor(&params[0]);
        zval_ptr_dtor(&params[1]);
        zval_ptr_dtor(&retval);
        zend_string_release(service_url);
        return FAILURE;
    }
    zval_ptr_dtor(&function_name);
    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&params[1]);
    zend_string_release(service_url);
    if (Z_TYPE(retval) != IS_ARRAY) {
        zval_ptr_dtor(&retval);
        zend_throw_exception(NULL, "CallCredentials plugin must return an array", 0);
        return FAILURE;
    }
    zend_hash_merge(Z_ARRVAL(call->metadata), Z_ARRVAL(retval), zval_add_ref, 0);
    zval_ptr_dtor(&retval);
    return SUCCESS;
}

static int grpc_lite_build_request_payload(zend_string *payload, zend_string **framed)
{
    smart_str request = {0};
    uint32_t len;
    if (ZSTR_LEN(payload) > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        return FAILURE;
    }
    len = htonl((uint32_t) ZSTR_LEN(payload));
    smart_str_appendc(&request, '\0');
    smart_str_appendl(&request, (const char *) &len, 4);
    smart_str_append(&request, payload);
    smart_str_0(&request);
    *framed = request.s != NULL ? request.s : zend_empty_string;
    return SUCCESS;
}

static int grpc_lite_extract_unary_payload(zval *result, zend_string **payload)
{
    zval *body_zv = zend_hash_str_find(Z_ARRVAL_P(result), "body", sizeof("body") - 1);
    const unsigned char *body;
    size_t body_len;
    uint32_t payload_len;
    if (body_zv == NULL || Z_TYPE_P(body_zv) != IS_STRING) {
        *payload = NULL;
        return SUCCESS;
    }
    body = (const unsigned char *) Z_STRVAL_P(body_zv);
    body_len = Z_STRLEN_P(body_zv);
    if (body_len == 0) {
        *payload = NULL;
        return SUCCESS;
    }
    if (body_len < 5 || body[0] != 0) {
        return FAILURE;
    }
    payload_len = ((uint32_t) body[1] << 24) | ((uint32_t) body[2] << 16) | ((uint32_t) body[3] << 8) | (uint32_t) body[4];
    if ((size_t) payload_len + 5 != body_len) {
        return FAILURE;
    }
    *payload = zend_string_init((const char *) body + 5, payload_len, 0);
    return SUCCESS;
}

static int grpc_lite_status_from_result(zval *result)
{
    zval *value;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "timed_out", sizeof("timed_out") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_DEADLINE_EXCEEDED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "cancelled", sizeof("cancelled") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_CANCELLED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "http_status", sizeof("http_status") - 1);
    if (value != NULL && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) == 503) return GRPC_STATUS_UNAVAILABLE;
    if (value != NULL && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) < 0) return GRPC_STATUS_UNAVAILABLE;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "invalid_content_type", sizeof("invalid_content_type") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_UNKNOWN;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "invalid_grpc_status", sizeof("invalid_grpc_status") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_UNKNOWN;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "response_message_too_large", sizeof("response_message_too_large") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_RESOURCE_EXHAUSTED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "metadata_too_large", sizeof("metadata_too_large") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_RESOURCE_EXHAUSTED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "malformed_response_frame", sizeof("malformed_response_frame") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_INTERNAL;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "compressed_response_seen", sizeof("compressed_response_seen") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_UNIMPLEMENTED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "unsupported_response_encoding", sizeof("unsupported_response_encoding") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_UNIMPLEMENTED;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "grpc_status", sizeof("grpc_status") - 1);
    if (value != NULL && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) >= 0) return (int) Z_LVAL_P(value);
    return GRPC_STATUS_UNKNOWN;
}

static bool grpc_lite_result_has_protocol_failure(zval *result)
{
    static const char *keys[] = {
        "response_message_too_large",
        "metadata_too_large",
        "invalid_content_type",
        "invalid_grpc_status",
        "malformed_response_frame",
        "compressed_response_seen",
        "unsupported_response_encoding",
    };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        zval *value = zend_hash_str_find(Z_ARRVAL_P(result), keys[i], strlen(keys[i]));
        if (value != NULL && zend_is_true(value)) {
            return true;
        }
    }
    return false;
}

static zend_string *grpc_lite_details_from_result(zval *result, int code)
{
    zval *message = zend_hash_str_find(Z_ARRVAL_P(result), "grpc_message", sizeof("grpc_message") - 1);
    zval *value;
    if (message != NULL && Z_TYPE_P(message) == IS_STRING && Z_STRLEN_P(message) > 0) {
        return zend_string_copy(Z_STR_P(message));
    }
    value = zend_hash_str_find(Z_ARRVAL_P(result), "http_status", sizeof("http_status") - 1);
    if (value != NULL && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) != 200) {
        return strpprintf(0, "HTTP status %ld without grpc-status", Z_LVAL_P(value));
    }
    value = zend_hash_str_find(Z_ARRVAL_P(result), "invalid_content_type", sizeof("invalid_content_type") - 1);
    if (value != NULL && zend_is_true(value)) {
        zval *content_type = zend_hash_str_find(Z_ARRVAL_P(result), "content_type", sizeof("content_type") - 1);
        if (content_type != NULL && Z_TYPE_P(content_type) == IS_STRING && Z_STRLEN_P(content_type) > 0) {
            return strpprintf(0, "invalid gRPC content-type: %s", Z_STRVAL_P(content_type));
        }
        return zend_string_init("invalid gRPC content-type", sizeof("invalid gRPC content-type") - 1, 0);
    }
    value = zend_hash_str_find(Z_ARRVAL_P(result), "invalid_grpc_status", sizeof("invalid_grpc_status") - 1);
    if (value != NULL && zend_is_true(value)) {
        return zend_string_init("invalid grpc-status trailer", sizeof("invalid grpc-status trailer") - 1, 0);
    }
    switch (code) {
        case GRPC_STATUS_DEADLINE_EXCEEDED:
            return zend_string_init("HTTP/2 transport deadline exceeded", sizeof("HTTP/2 transport deadline exceeded") - 1, 0);
        case GRPC_STATUS_RESOURCE_EXHAUSTED:
            return zend_string_init("received message exceeds maximum size", sizeof("received message exceeds maximum size") - 1, 0);
        case GRPC_STATUS_INTERNAL:
            return zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
        case GRPC_STATUS_UNIMPLEMENTED:
            value = zend_hash_str_find(Z_ARRVAL_P(result), "unsupported_response_encoding", sizeof("unsupported_response_encoding") - 1);
            if (value != NULL && zend_is_true(value)) {
                zval *encoding = zend_hash_str_find(Z_ARRVAL_P(result), "grpc_encoding", sizeof("grpc_encoding") - 1);
                if (encoding != NULL && Z_TYPE_P(encoding) == IS_STRING && Z_STRLEN_P(encoding) > 0) {
                    return strpprintf(0, "unsupported grpc-encoding: %s", Z_STRVAL_P(encoding));
                }
                return zend_string_init("unsupported grpc-encoding", sizeof("unsupported grpc-encoding") - 1, 0);
            }
            return zend_string_init("compressed gRPC messages are not supported", sizeof("compressed gRPC messages are not supported") - 1, 0);
        case GRPC_STATUS_CANCELLED:
            return zend_string_init("Cancelled", sizeof("Cancelled") - 1, 0);
        default:
            return zend_empty_string;
    }
}

static void grpc_lite_make_status_object(zval *status, int code, zend_string *details, zval *metadata)
{
    object_init(status);
    add_property_long(status, "code", code);
    add_property_str(status, "details", zend_string_copy(details));
    if (metadata != NULL && Z_TYPE_P(metadata) == IS_ARRAY) {
        zval copy;
        array_init(&copy);
        zend_hash_copy(Z_ARRVAL(copy), Z_ARRVAL_P(metadata), zval_add_ref);
        add_property_zval(status, "metadata", &copy);
        zval_ptr_dtor(&copy);
    } else {
        zval empty;
        array_init(&empty);
        add_property_zval(status, "metadata", &empty);
        zval_ptr_dtor(&empty);
    }
}

static int grpc_lite_channel_key(grpc_lite_channel_obj *channel, zend_string **key)
{
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    *key = strpprintf(0, "%s|%ld|%s|%s|%d|%zu|%zu|%zu",
        ZSTR_VAL(channel->host),
        channel->port,
        channel->authority != NULL ? ZSTR_VAL(channel->authority) : "",
        channel->tls_verify_name != NULL ? ZSTR_VAL(channel->tls_verify_name) : "",
        credentials->type,
        credentials->root_certs != NULL ? ZSTR_LEN(credentials->root_certs) : 0,
        credentials->cert_chain != NULL ? ZSTR_LEN(credentials->cert_chain) : 0,
        credentials->private_key != NULL ? ZSTR_LEN(credentials->private_key) : 0);
    return SUCCESS;
}

static int grpc_lite_perform_call_unary(grpc_lite_call_obj *call)
{
    grpc_lite_channel_obj *channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    zend_string *key = NULL;
    zend_string *framed = NULL;
    h2_channel *h2;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long timeout_us = grpc_lite_call_timeout_us(call);
    zval result;
    zend_string *payload = NULL;
    int status_code;
    zend_string *details;
    zval *initial_metadata;
    zval *trailing_metadata;

    if (call->request == NULL) {
        zend_throw_exception(NULL, "Call has no request message", 0);
        return FAILURE;
    }
    if (grpc_lite_build_request_payload(call->request, &framed) != SUCCESS) {
        return FAILURE;
    }
    if (grpc_lite_merge_call_credentials_metadata(call, channel) != SUCCESS) {
        zend_string_release(framed);
        return FAILURE;
    }
    grpc_lite_append_timeout_metadata(&call->metadata, timeout_us);
    grpc_lite_append_user_agent(channel, &call->metadata);
    grpc_lite_channel_key(channel, &key);
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    h2 = get_persistent_channel(
        ZSTR_VAL(key),
        ZSTR_LEN(key),
        ZSTR_VAL(channel->host),
        channel->port,
        channel->authority != NULL ? ZSTR_VAL(channel->authority) : NULL,
        channel->authority != NULL ? ZSTR_LEN(channel->authority) : 0,
        channel->tls_verify_name != NULL ? ZSTR_VAL(channel->tls_verify_name) : NULL,
        channel->tls_verify_name != NULL ? ZSTR_LEN(channel->tls_verify_name) : 0,
        credentials->type != GRPC_LITE_CREDENTIALS_INSECURE,
        credentials->root_certs != NULL ? ZSTR_VAL(credentials->root_certs) : NULL,
        credentials->root_certs != NULL ? ZSTR_LEN(credentials->root_certs) : 0,
        credentials->cert_chain != NULL ? ZSTR_VAL(credentials->cert_chain) : NULL,
        credentials->cert_chain != NULL ? ZSTR_LEN(credentials->cert_chain) : 0,
        credentials->private_key != NULL ? ZSTR_VAL(credentials->private_key) : NULL,
        credentials->private_key != NULL ? ZSTR_LEN(credentials->private_key) : 0,
        deadline_abs_us,
        error_detail,
        sizeof(error_detail),
        &persistent_reused,
        &error_message);
    if (h2 == NULL) {
        int code = (timeout_us > 0 && timeout_us <= 1) ? GRPC_STATUS_DEADLINE_EXCEEDED : GRPC_STATUS_UNAVAILABLE;
        zend_string *details = zend_string_init(error_message != NULL ? error_message : "failed to open persistent channel", strlen(error_message != NULL ? error_message : "failed to open persistent channel"), 0);
        zval_ptr_dtor(&call->initial_metadata);
        array_init(&call->initial_metadata);
        zval_ptr_dtor(&call->trailing_metadata);
        array_init(&call->trailing_metadata);
        zval_ptr_dtor(&call->status);
        grpc_lite_make_status_object(&call->status, code, details, &call->trailing_metadata);
        call->initial_metadata_ready = true;
        call->status_ready = true;
        call->unary_performed = true;
        if (call->request != NULL) {
            zend_string_release(call->request);
            call->request = NULL;
        }
        zend_string_release(details);
        zend_string_release(framed);
        zend_string_release(key);
        return SUCCESS;
    }
    array_init(&result);
    if (perform_h2_channel_unary(h2, ZSTR_VAL(call->method), ZSTR_LEN(call->method), ZSTR_VAL(framed), ZSTR_LEN(framed), &call->metadata, timeout_us, channel->max_receive_message_length, true, persistent_reused, &result) != SUCCESS) {
        zend_string_release(framed);
        zend_string_release(key);
        zval_ptr_dtor(&result);
        return FAILURE;
    }
    if (!channel_usable(h2)) {
        remove_unusable_persistent_channel(ZSTR_VAL(key), ZSTR_LEN(key), h2);
    }
    status_code = grpc_lite_status_from_result(&result);
    if (grpc_lite_result_has_protocol_failure(&result)) {
        detach_persistent_channel_by_ptr(h2);
    }
    details = grpc_lite_details_from_result(&result, status_code);
    initial_metadata = zend_hash_str_find(Z_ARRVAL(result), "initial_metadata", sizeof("initial_metadata") - 1);
    trailing_metadata = zend_hash_str_find(Z_ARRVAL(result), "trailing_metadata", sizeof("trailing_metadata") - 1);
    grpc_lite_copy_metadata(&call->initial_metadata, initial_metadata);
    grpc_lite_copy_metadata(&call->trailing_metadata, trailing_metadata);
    zval_ptr_dtor(&call->status);
    grpc_lite_make_status_object(&call->status, status_code, details, &call->trailing_metadata);
    call->initial_metadata_ready = true;
    call->status_ready = true;
    if (status_code == GRPC_STATUS_OK && grpc_lite_extract_unary_payload(&result, &payload) != SUCCESS) {
        status_code = GRPC_STATUS_INTERNAL;
        zend_string_release(details);
        details = zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
        zval_ptr_dtor(&call->status);
        grpc_lite_make_status_object(&call->status, status_code, details, &call->trailing_metadata);
    }
    if (payload != NULL) {
        if (call->request != NULL) {
            zend_string_release(call->request);
        }
        call->request = payload;
    } else {
        if (call->request != NULL) {
            zend_string_release(call->request);
            call->request = NULL;
        }
    }
    call->unary_performed = true;
    zend_string_release(details);
    zend_string_release(framed);
    zend_string_release(key);
    zval_ptr_dtor(&result);
    return SUCCESS;
}

static int grpc_lite_open_call_stream(grpc_lite_call_obj *call)
{
    grpc_lite_channel_obj *channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    zend_string *key = NULL;
    zend_long timeout_us = grpc_lite_call_timeout_us(call);
    zval function_name;
    zval params[14];
    zval stream_result;
    int param_count = 0;
    int i;

    if (call->stream_opened) {
        return SUCCESS;
    }
    if (call->request == NULL) {
        zend_throw_exception(NULL, "Call has no request message", 0);
        return FAILURE;
    }
    if (grpc_lite_merge_call_credentials_metadata(call, channel) != SUCCESS) {
        return FAILURE;
    }
    grpc_lite_append_timeout_metadata(&call->metadata, timeout_us);
    grpc_lite_append_user_agent(channel, &call->metadata);
    grpc_lite_channel_key(channel, &key);

    zval_ptr_dtor(&call->stream);
    ZVAL_STRING(&function_name, "grpc_lite_stream_open");
    ZVAL_STR_COPY(&params[param_count++], key);
    ZVAL_STR_COPY(&params[param_count++], channel->host);
    ZVAL_LONG(&params[param_count++], channel->port);
    ZVAL_STR_COPY(&params[param_count++], call->method);
    ZVAL_STR_COPY(&params[param_count++], call->request);
    ZVAL_COPY(&params[param_count++], &call->metadata);
    ZVAL_LONG(&params[param_count++], timeout_us);
    ZVAL_BOOL(&params[param_count++], credentials->type != GRPC_LITE_CREDENTIALS_INSECURE);
    if (credentials->root_certs != NULL) ZVAL_STR_COPY(&params[param_count++], credentials->root_certs); else ZVAL_NULL(&params[param_count++]);
    if (credentials->cert_chain != NULL) ZVAL_STR_COPY(&params[param_count++], credentials->cert_chain); else ZVAL_NULL(&params[param_count++]);
    if (credentials->private_key != NULL) ZVAL_STR_COPY(&params[param_count++], credentials->private_key); else ZVAL_NULL(&params[param_count++]);
    ZVAL_LONG(&params[param_count++], channel->max_receive_message_length);
    if (channel->authority != NULL) ZVAL_STR_COPY(&params[param_count++], channel->authority); else ZVAL_NULL(&params[param_count++]);
    if (channel->tls_verify_name != NULL) ZVAL_STR_COPY(&params[param_count++], channel->tls_verify_name); else ZVAL_NULL(&params[param_count++]);
    ZVAL_UNDEF(&stream_result);
    if (call_user_function(EG(function_table), NULL, &function_name, &stream_result, param_count, params) != SUCCESS || EG(exception)) {
        for (i = 0; i < param_count; i++) {
            zval_ptr_dtor(&params[i]);
        }
        zval_ptr_dtor(&function_name);
        zend_string_release(key);
        return FAILURE;
    }
    for (i = 0; i < param_count; i++) {
        zval_ptr_dtor(&params[i]);
    }
    zval_ptr_dtor(&function_name);
    ZVAL_COPY_VALUE(&call->stream, &stream_result);
    call->stream_opened = true;
    zend_string_release(key);
    return SUCCESS;
}

static int grpc_lite_stream_next_for_call(grpc_lite_call_obj *call, zval *result)
{
    zval function_name;
    zval params[1];
    if (!call->stream_opened && grpc_lite_open_call_stream(call) != SUCCESS) {
        return FAILURE;
    }
    ZVAL_STRING(&function_name, "grpc_lite_stream_next");
    ZVAL_COPY(&params[0], &call->stream);
    if (call_user_function(EG(function_table), NULL, &function_name, result, 1, params) != SUCCESS || EG(exception)) {
        zval_ptr_dtor(&params[0]);
        zval_ptr_dtor(&function_name);
        return FAILURE;
    }
    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&function_name);
    return SUCCESS;
}

static void grpc_lite_add_event_metadata(zval *event, zval *metadata)
{
    zval copy;
    array_init(&copy);
    if (metadata != NULL && Z_TYPE_P(metadata) == IS_ARRAY) {
        zend_hash_copy(Z_ARRVAL(copy), Z_ARRVAL_P(metadata), zval_add_ref);
    }
    add_property_zval(event, "metadata", &copy);
    zval_ptr_dtor(&copy);
}

static void grpc_lite_add_event_message(zval *event, zend_string *message)
{
    if (message != NULL) {
        add_property_str(event, "message", zend_string_copy(message));
    } else {
        add_property_null(event, "message");
    }
}

static void grpc_lite_add_event_status(zval *event, zval *status)
{
    if (status != NULL && Z_TYPE_P(status) == IS_OBJECT) {
        add_property_zval(event, "status", status);
    }
}

static void grpc_lite_mark_call_cancelled(grpc_lite_call_obj *call)
{
    zend_string *details;
    if (call->status_ready) {
        return;
    }
    zval_ptr_dtor(&call->initial_metadata);
    array_init(&call->initial_metadata);
    zval_ptr_dtor(&call->trailing_metadata);
    array_init(&call->trailing_metadata);
    details = zend_string_init("Cancelled", sizeof("Cancelled") - 1, 0);
    zval_ptr_dtor(&call->status);
    grpc_lite_make_status_object(&call->status, GRPC_STATUS_CANCELLED, details, &call->trailing_metadata);
    zend_string_release(details);
    call->initial_metadata_ready = true;
    call->status_ready = true;
    call->unary_performed = true;
}

static int grpc_lite_store_send_batch(grpc_lite_call_obj *call, zval *ops)
{
    zval *metadata = zend_hash_index_find(Z_ARRVAL_P(ops), GRPC_OP_SEND_INITIAL_METADATA);
    zval *message = zend_hash_index_find(Z_ARRVAL_P(ops), GRPC_OP_SEND_MESSAGE);
    zval *payload;

    if (metadata != NULL) {
        grpc_lite_copy_metadata(&call->metadata, metadata);
    }
    if (message != NULL) {
        if (Z_TYPE_P(message) != IS_ARRAY) {
            zend_throw_exception(NULL, "OP_SEND_MESSAGE expects an array", 0);
            return FAILURE;
        }
        payload = zend_hash_str_find(Z_ARRVAL_P(message), "message", sizeof("message") - 1);
        if (payload == NULL || Z_TYPE_P(payload) != IS_STRING) {
            zend_throw_exception(NULL, "OP_SEND_MESSAGE expects a string message", 0);
            return FAILURE;
        }
        if (call->request != NULL) {
            zend_string_release(call->request);
        }
        call->request = zend_string_copy(Z_STR_P(payload));
    }
    call->sent = true;
    return SUCCESS;
}

PHP_METHOD(Call, startBatch)
{
    zval *ops;
    grpc_lite_call_obj *call = Z_GRPC_LITE_CALL_P(ZEND_THIS);
    bool wants_initial_metadata = false;
    bool wants_message = false;
    bool wants_status = false;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(ops)
    ZEND_PARSE_PARAMETERS_END();

    if (zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_SEND_INITIAL_METADATA) ||
        zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_SEND_MESSAGE) ||
        zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_SEND_CLOSE_FROM_CLIENT)) {
        if (grpc_lite_store_send_batch(call, ops) != SUCCESS) {
            RETURN_THROWS();
        }
    }

    wants_initial_metadata = zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_RECV_INITIAL_METADATA);
    wants_message = zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_RECV_MESSAGE);
    wants_status = zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_RECV_STATUS_ON_CLIENT);

    object_init(return_value);

    if (call->cancelled) {
        grpc_lite_mark_call_cancelled(call);
        if (wants_initial_metadata) {
            grpc_lite_add_event_metadata(return_value, &call->initial_metadata);
        }
        if (wants_message) {
            grpc_lite_add_event_message(return_value, NULL);
        }
        if (wants_status) {
            grpc_lite_add_event_status(return_value, &call->status);
        }
        return;
    }

    if (wants_status) {
        if (!call->unary_performed && !call->stream_opened) {
            if (grpc_lite_perform_call_unary(call) != SUCCESS) {
                RETURN_THROWS();
            }
        } else if (call->stream_opened && !call->status_ready) {
            zval result;
            do {
                ZVAL_UNDEF(&result);
                if (grpc_lite_stream_next_for_call(call, &result) != SUCCESS) {
                    RETURN_THROWS();
                }
                if (Z_TYPE(result) == IS_ARRAY) {
                    zval *done = zend_hash_str_find(Z_ARRVAL(result), "done", sizeof("done") - 1);
                    if (done != NULL && zend_is_true(done)) {
                        int code = grpc_lite_status_from_result(&result);
                        zend_string *details = grpc_lite_details_from_result(&result, code);
                        zval *initial_metadata = zend_hash_str_find(Z_ARRVAL(result), "initial_metadata", sizeof("initial_metadata") - 1);
                        zval *trailing_metadata = zend_hash_str_find(Z_ARRVAL(result), "trailing_metadata", sizeof("trailing_metadata") - 1);
                        if (!call->initial_metadata_ready) {
                            grpc_lite_copy_metadata(&call->initial_metadata, initial_metadata);
                            call->initial_metadata_ready = true;
                        }
                        grpc_lite_copy_metadata(&call->trailing_metadata, trailing_metadata);
                        zval_ptr_dtor(&call->status);
                        grpc_lite_make_status_object(&call->status, code, details, &call->trailing_metadata);
                        call->status_ready = true;
                        zend_string_release(details);
                    }
                }
                zval_ptr_dtor(&result);
            } while (!call->status_ready);
        }
    } else if (wants_message && !call->unary_performed) {
        if (call->stream_opened || !zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_RECV_STATUS_ON_CLIENT)) {
            zval result;
            ZVAL_UNDEF(&result);
            if (grpc_lite_stream_next_for_call(call, &result) != SUCCESS) {
                RETURN_THROWS();
            }
            if (Z_TYPE(result) == IS_ARRAY) {
                zval *done = zend_hash_str_find(Z_ARRVAL(result), "done", sizeof("done") - 1);
                if (done != NULL && zend_is_true(done)) {
                    int code = grpc_lite_status_from_result(&result);
                    zend_string *details = grpc_lite_details_from_result(&result, code);
                    zval *initial_metadata = zend_hash_str_find(Z_ARRVAL(result), "initial_metadata", sizeof("initial_metadata") - 1);
                    zval *trailing_metadata = zend_hash_str_find(Z_ARRVAL(result), "trailing_metadata", sizeof("trailing_metadata") - 1);
                    if (!call->initial_metadata_ready) {
                        grpc_lite_copy_metadata(&call->initial_metadata, initial_metadata);
                        call->initial_metadata_ready = true;
                    }
                    grpc_lite_copy_metadata(&call->trailing_metadata, trailing_metadata);
                    zval_ptr_dtor(&call->status);
                    grpc_lite_make_status_object(&call->status, code, details, &call->trailing_metadata);
                    call->status_ready = true;
                    grpc_lite_add_event_message(return_value, NULL);
                    zend_string_release(details);
                } else {
                    zval *payload = zend_hash_str_find(Z_ARRVAL(result), "payload", sizeof("payload") - 1);
                    zval *initial_metadata = zend_hash_str_find(Z_ARRVAL(result), "initial_metadata", sizeof("initial_metadata") - 1);
                    if (!call->initial_metadata_ready) {
                        grpc_lite_copy_metadata(&call->initial_metadata, initial_metadata);
                        call->initial_metadata_ready = true;
                    }
                    grpc_lite_add_event_message(return_value, payload != NULL && Z_TYPE_P(payload) == IS_STRING ? Z_STR_P(payload) : NULL);
                }
            }
            zval_ptr_dtor(&result);
        } else {
            if (grpc_lite_perform_call_unary(call) != SUCCESS) {
                RETURN_THROWS();
            }
        }
    }

    if (wants_initial_metadata) {
        grpc_lite_add_event_metadata(return_value, &call->initial_metadata);
    }
    if (wants_message && call->unary_performed) {
        grpc_lite_add_event_message(return_value, call->request);
    } else if (wants_message && !zend_hash_str_exists(Z_OBJPROP_P(return_value), "message", sizeof("message") - 1)) {
        grpc_lite_add_event_message(return_value, NULL);
    }
    if (wants_status) {
        grpc_lite_add_event_status(return_value, &call->status);
    }
}

PHP_METHOD(Call, cancel)
{
    grpc_lite_call_obj *call = Z_GRPC_LITE_CALL_P(ZEND_THIS);
    call->cancelled = true;
    if (call->stream_opened && Z_TYPE(call->stream) == IS_RESOURCE) {
        zval function_name;
        zval params[1];
        zval result;
        ZVAL_STRING(&function_name, "grpc_lite_stream_cancel");
        ZVAL_COPY(&params[0], &call->stream);
        ZVAL_UNDEF(&result);
        call_user_function(EG(function_table), NULL, &function_name, &result, 1, params);
        zval_ptr_dtor(&params[0]);
        zval_ptr_dtor(&function_name);
        zval_ptr_dtor(&result);
    }
}

PHP_METHOD(Call, getPeer)
{
    grpc_lite_call_obj *call = Z_GRPC_LITE_CALL_P(ZEND_THIS);
    grpc_lite_channel_obj *channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    RETURN_STR_COPY(channel->target);
}


PHP_FUNCTION(grpc_lite_unary)
{
    char *key = NULL;
    size_t key_len = 0;
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    zend_long timeout_us = 0;
    bool use_tls = false;
    char *root_certs = NULL;
    size_t root_certs_len = 0;
    char *cert_chain = NULL;
    size_t cert_chain_len = 0;
    char *private_key = NULL;
    size_t private_key_len = 0;
    zend_long max_receive_message_length = 0;
    char *authority = NULL;
    size_t authority_len = 0;
    char *tls_verify_name = NULL;
    size_t tls_verify_name_len = 0;
    h2_channel *channel;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;

    ZEND_PARSE_PARAMETERS_START(5, 14)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_LONG(timeout_us)
        Z_PARAM_BOOL(use_tls)
        Z_PARAM_STRING_OR_NULL(root_certs, root_certs_len)
        Z_PARAM_STRING_OR_NULL(cert_chain, cert_chain_len)
        Z_PARAM_STRING_OR_NULL(private_key, private_key_len)
        Z_PARAM_LONG(max_receive_message_length)
        Z_PARAM_STRING_OR_NULL(authority, authority_len)
        Z_PARAM_STRING_OR_NULL(tls_verify_name, tls_verify_name_len)
    ZEND_PARSE_PARAMETERS_END();

    error_message = validate_channel_inputs(key, key_len, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }
    if (timeout_us < 0) {
        zend_throw_exception(NULL, "timeout must be non-negative microseconds", 0);
        RETURN_THROWS();
    }
    error_message = validate_grpc_path(path, path_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }

    if (!PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        zend_throw_exception(NULL, "persistent channel cache is not initialized", 0);
        RETURN_THROWS();
    }

    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    channel = get_persistent_channel(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }

    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        RETURN_THROWS();
    }

    if (perform_h2_channel_unary(channel, path, path_len, request, request_len, headers_zv, remaining_timeout_us, max_receive_message_length, true, persistent_reused, return_value) != SUCCESS) {
        if (channel != NULL && !channel_usable(channel)) {
            remove_unusable_persistent_channel(key, key_len, channel);
        }
        RETURN_THROWS();
    }

    if (!channel_usable(channel)) {
        remove_unusable_persistent_channel(key, key_len, channel);
    }
}

PHP_FUNCTION(grpc_lite_stream_open)
{
    char *key = NULL;
    size_t key_len = 0;
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    zend_long timeout_us = 0;
    bool use_tls = false;
    char *root_certs = NULL;
    size_t root_certs_len = 0;
    char *cert_chain = NULL;
    size_t cert_chain_len = 0;
    char *private_key = NULL;
    size_t private_key_len = 0;
    zend_long max_receive_message_length = 0;
    char *authority = NULL;
    size_t authority_len = 0;
    char *tls_verify_name = NULL;
    size_t tls_verify_name_len = 0;
    h2_channel *channel;
    h2_stream *stream;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;
    int rv;

    ZEND_PARSE_PARAMETERS_START(5, 14)
        Z_PARAM_STRING(key, key_len)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_LONG(timeout_us)
        Z_PARAM_BOOL(use_tls)
        Z_PARAM_STRING_OR_NULL(root_certs, root_certs_len)
        Z_PARAM_STRING_OR_NULL(cert_chain, cert_chain_len)
        Z_PARAM_STRING_OR_NULL(private_key, private_key_len)
        Z_PARAM_LONG(max_receive_message_length)
        Z_PARAM_STRING_OR_NULL(authority, authority_len)
        Z_PARAM_STRING_OR_NULL(tls_verify_name, tls_verify_name_len)
    ZEND_PARSE_PARAMETERS_END();

    error_message = validate_channel_inputs(key, key_len, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }
    if (timeout_us < 0) {
        zend_throw_exception(NULL, "timeout must be non-negative microseconds", 0);
        RETURN_THROWS();
    }
    error_message = validate_grpc_path(path, path_len);
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }

    if (request_len > UINT32_MAX) {
        zend_throw_exception(NULL, "gRPC request message exceeds 32-bit frame length", 0);
        RETURN_THROWS();
    }
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    channel = get_persistent_channel(key, key_len, host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (channel == NULL) {
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent channel", 0);
        RETURN_THROWS();
    }
    if (channel->busy) {
        zend_throw_exception(NULL, "HTTP/2 channel already has an active stream", 0);
        RETURN_THROWS();
    }
    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        RETURN_THROWS();
    }
    if (set_socket_timeout_us(channel->fd, remaining_timeout_us) != 0) {
        mark_channel_dead(channel, errno);
        discard_persistent_channel(key, key_len, channel);
        zend_throw_exception(NULL, "failed to set socket timeout", 0);
        RETURN_THROWS();
    }

    stream = ecalloc(1, sizeof(h2_stream));
    stream->channel = channel;
    stream->request = zend_string_init(request, request_len, 0);
    stream->recv_buf_len = 65536;
    stream->recv_buf = emalloc(stream->recv_buf_len);

    memset(&stream->client, 0, sizeof(stream->client));
    stream->client.fd = channel->fd;
    stream->client.channel = channel;
    stream->client.grpc_status = -1;
    stream->client.http_status = -1;
    stream->client.request = (const uint8_t *) ZSTR_VAL(stream->request);
    stream->client.request_len = ZSTR_LEN(stream->request);
    stream->client.max_receive_message_bytes = effective_max_receive_message_bytes(max_receive_message_length);
    stream->client.deadline_abs_us = deadline_abs_us > 0 ? deadline_abs_us : 0;
    stream->client.decode_response_incrementally = true;
    stream->client.direct_response_payload = true;
    stream->client.queue_response_payloads = true;
    set_grpc_header(&stream->client, stream->client.request_len);

    nghttp2_session_set_user_data(channel->session, &stream->client);
    channel->busy = true;
    channel->active_stream_owner = stream;
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        destroy_h2_stream(stream);
        RETURN_THROWS();
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, channel->tls ? "https" : "http", channel->tls ? sizeof("https") - 1 : sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, channel->authority, strlen(channel->authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        RETURN_THROWS();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    stream->client.stream_id = nghttp2_submit_request(channel->session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
    if (stream->client.stream_id < 0) {
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        RETURN_THROWS();
    }

    rv = nghttp2_session_send(channel->session);
    if (rv != 0) {
        bool stream_timed_out = stream->client.timed_out;
        mark_channel_dead(channel, rv);
        free_request_headers(&request_headers);
        destroy_h2_stream(stream);
        if (stream_timed_out) {
            zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
            RETURN_THROWS();
        }
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        RETURN_THROWS();
    }

    free_request_headers(&request_headers);
    RETURN_RES(zend_register_resource(stream, le_h2_stream));
}

static void add_stream_status(zval *return_value, h2_stream *stream)
{
    grpc_call *client = &stream->client;
    add_assoc_bool(return_value, "done", true);
    add_assoc_long(return_value, "grpc_status", client->grpc_status);
    add_assoc_str(return_value, "grpc_message", client->grpc_message != NULL ? zend_string_copy(client->grpc_message) : zend_empty_string);
    add_assoc_long(return_value, "http_status", client->http_status);
    add_assoc_long(return_value, "stream_error_code", client->stream_error_code);
    add_assoc_bool(return_value, "stream_reset_seen", client->stream_reset_seen);
    add_assoc_bool(return_value, "invalid_grpc_status", client->invalid_grpc_status);
    add_assoc_str(return_value, "content_type", client->content_type != NULL ? zend_string_copy(client->content_type) : zend_empty_string);
    add_assoc_str(return_value, "grpc_encoding", client->grpc_encoding != NULL ? zend_string_copy(client->grpc_encoding) : zend_empty_string);
    add_assoc_bool(return_value, "compressed_response_seen", client->compressed_response_seen);
    add_assoc_bool(return_value, "response_message_too_large", client->response_message_too_large);
    add_assoc_bool(return_value, "malformed_response_frame", client->malformed_response_frame);
    add_assoc_bool(return_value, "metadata_too_large", client->metadata_too_large);
    add_assoc_bool(return_value, "invalid_content_type", client->invalid_content_type);
    add_assoc_bool(return_value, "unsupported_response_encoding", client->unsupported_response_encoding);
    add_assoc_long(return_value, "max_receive_message_length", client->max_receive_message_bytes > (size_t) ZEND_LONG_MAX ? ZEND_LONG_MAX : (zend_long) client->max_receive_message_bytes);
    add_assoc_bool(return_value, "timed_out", client->timed_out);
    add_assoc_bool(return_value, "cancelled", stream->cancelled);
    add_assoc_long(return_value, "body_bytes", 0);
    add_assoc_long(return_value, "bytes_sent", client->bytes_sent);
    add_assoc_long(return_value, "bytes_received", client->bytes_received);
    add_assoc_bool(return_value, "channel_dead", stream->channel != NULL ? stream->channel->dead : false);
    add_assoc_bool(return_value, "channel_draining", stream->channel != NULL ? stream->channel->draining : false);
    add_assoc_long(return_value, "channel_last_error", stream->channel != NULL ? stream->channel->last_error : 0);
    add_assoc_long(return_value, "channel_last_io_errno", stream->channel != NULL ? stream->channel->last_io_errno : client->last_io_errno);
    add_assoc_long(return_value, "channel_last_ssl_error", stream->channel != NULL ? stream->channel->last_ssl_error : client->last_ssl_error);
    add_assoc_long(return_value, "channel_tls_verify_result", stream->channel != NULL ? (zend_long) stream->channel->tls_verify_result : 0);
    add_assoc_string(return_value, "channel_last_error_detail", stream->channel != NULL ? stream->channel->last_error_detail : client->last_io_error_detail);
    add_assoc_string(return_value, "channel_negotiated_protocol", stream->channel != NULL ? stream->channel->negotiated_protocol : "");
    add_metadata_map_to_return(return_value, "initial_metadata", client, false);
    add_metadata_map_to_return(return_value, "trailing_metadata", client, true);
}

PHP_FUNCTION(grpc_lite_stream_next)
{
    zval *stream_zv = NULL;
    h2_stream *stream;
    grpc_call *client;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (h2_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_stream", le_h2_stream);
    if (stream == NULL) {
        RETURN_THROWS();
    }
    client = &stream->client;

    array_init(return_value);

    while (client->response_queue_head == NULL && !client->stream_closed && !stream->completed && !client->response_message_too_large && !client->compressed_response_seen && !client->malformed_response_frame && !client->invalid_content_type && !client->unsupported_response_encoding && !client->metadata_too_large) {
        int rv;
        ssize_t nread;
        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            cancel_active_stream(stream, NGHTTP2_CANCEL);
            stream->completed = true;
            break;
        }
        rv = nghttp2_session_send(stream->channel->session);
        if (rv != 0) {
            mark_channel_dead(stream->channel, rv);
            if (client->timed_out) {
                stream->completed = true;
                break;
            }
            stream->completed = true;
            break;
        }
        nread = channel_recv(stream->channel, (uint8_t *) stream->recv_buf, stream->recv_buf_len, client->deadline_abs_us);
        if (nread <= 0) {
            bool socket_timeout = nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) && client->deadline_abs_us > 0;
            if (nread < 0) {
                client->last_io_errno = errno;
                client->last_ssl_error = stream->channel->last_ssl_error;
                snprintf(client->last_io_error_detail, sizeof(client->last_io_error_detail), "%s", stream->channel->last_error_detail);
            }
            if (socket_timeout) {
                client->timed_out = true;
                cancel_active_stream(stream, NGHTTP2_CANCEL);
            } else {
                mark_channel_dead(stream->channel, nread == 0 ? 0 : errno);
            }
            stream->completed = true;
            break;
        }
        client->bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(stream->channel->session, (const uint8_t *) stream->recv_buf, (size_t) nread);
        if (rv < 0) {
            mark_channel_dead(stream->channel, rv);
            stream->completed = true;
            break;
        }
    }

    if (client->response_message_too_large || client->compressed_response_seen || client->malformed_response_frame || client->invalid_content_type || client->unsupported_response_encoding || client->metadata_too_large) {
        terminate_stream_with_cancel(stream);
        detach_persistent_channel_by_ptr(stream->channel);
    }

    if (client->response_queue_head != NULL) {
        queued_payload *entry = client->response_queue_head;
        client->response_queue_head = entry->next;
        if (client->response_queue_head == NULL) {
            client->response_queue_tail = NULL;
        }
        client->response_queue_count--;
        client->response_queue_bytes -= ZSTR_LEN(entry->payload);
        add_assoc_bool(return_value, "done", false);
        add_assoc_str(return_value, "payload", entry->payload);
        add_metadata_map_to_return(return_value, "initial_metadata", client, false);
        efree(entry);
        return;
    }

    if (!client->response_message_too_large && !client->compressed_response_seen && (client->response_header_len != 0 || client->response_payload != NULL || client->response_payload_offset != 0)) {
        client->malformed_response_frame = true;
    }
    stream->completed = true;
    add_stream_status(return_value, stream);
    clear_channel_stream_owner(stream);
}

PHP_FUNCTION(grpc_lite_stream_cancel)
{
    zval *stream_zv = NULL;
    h2_stream *stream;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();

    stream = (h2_stream *) zend_fetch_resource(Z_RES_P(stream_zv), "grpc_lite_stream", le_h2_stream);
    if (stream == NULL) {
        RETURN_THROWS();
    }
    if (stream != NULL && !stream->completed && channel_owned_by_stream(stream->channel, stream) && channel_usable(stream->channel)) {
        stream->cancelled = true;
        stream->client.grpc_status = 1;
        terminate_stream_with_cancel(stream);
        clear_channel_stream_owner(stream);
    }

    RETURN_TRUE;
}

PHP_FUNCTION(grpc_lite_channel_close)
{
    char *key = NULL;
    size_t key_len = 0;
    h2_channel *channel;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(key, key_len)
    ZEND_PARSE_PARAMETERS_END();

    if (!PHP_GRPC_LITE_G(persistent_channels_initialized)) {
        RETURN_FALSE;
    }

    channel = zend_hash_str_find_ptr(&PHP_GRPC_LITE_G(persistent_channels), key, key_len);
    if (channel == NULL) {
        RETURN_FALSE;
    }

    discard_persistent_channel(key, key_len, channel);
    RETURN_TRUE;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
#include "grpc_bench.c"
#endif

PHP_GINIT_FUNCTION(grpc_lite)
{
#if defined(COMPILE_DL_GRPC) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_init(&grpc_lite_globals->persistent_channels, 8, NULL, NULL, 1);
    grpc_lite_globals->persistent_channels_initialized = true;
}

PHP_GSHUTDOWN_FUNCTION(grpc_lite)
{
    h2_channel *channel;

    if (!grpc_lite_globals->persistent_channels_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&grpc_lite_globals->persistent_channels, channel) {
        destroy_h2_channel(channel);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&grpc_lite_globals->persistent_channels);
    grpc_lite_globals->persistent_channels_initialized = false;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_no_args, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_construct, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, target, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, opts, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_get_connectivity_state, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, try_to_connect, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_watch_connectivity_state, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, last_observed_state, IS_LONG, 0)
    ZEND_ARG_OBJ_INFO(0, deadline, Grpc\\Timeval, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_call_construct, 0, 0, 3)
    ZEND_ARG_OBJ_INFO(0, channel, Grpc\\Channel, 0)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO(0, deadline, Grpc\\Timeval, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_call_start_batch, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, batch, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_call_set_credentials, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, credentials, Grpc\\CallCredentials, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_credentials_create_ssl, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_credentials_set_roots, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, pem_roots, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_call_credentials_create_plugin, 0, 0, 1)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_timeval_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, microseconds, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_timeval_other, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, other, Grpc\\Timeval, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry channel_credentials_methods[] = {
    PHP_ME(ChannelCredentials, createInsecure, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(ChannelCredentials, createSsl, arginfo_channel_credentials_create_ssl, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(ChannelCredentials, createDefault, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(ChannelCredentials, setDefaultRootsPem, arginfo_channel_credentials_set_roots, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(ChannelCredentials, isDefaultRootsPemSet, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(ChannelCredentials, invalidateDefaultRootsPem, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry call_credentials_methods[] = {
    PHP_ME(CallCredentials, createFromPlugin, arginfo_call_credentials_create_plugin, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry timeval_methods[] = {
    PHP_ME(Timeval, __construct, arginfo_timeval_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Timeval, now, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Timeval, infFuture, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Timeval, infPast, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Timeval, zero, arginfo_no_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Timeval, add, arginfo_timeval_other, ZEND_ACC_PUBLIC)
    PHP_ME(Timeval, subtract, arginfo_timeval_other, ZEND_ACC_PUBLIC)
    PHP_ME(Timeval, compare, arginfo_timeval_other, ZEND_ACC_PUBLIC)
    PHP_ME(Timeval, microtime, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_ME(Timeval, sleepUntil, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry channel_methods[] = {
    PHP_ME(Channel, __construct, arginfo_channel_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Channel, getTarget, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_ME(Channel, close, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_ME(Channel, getConnectivityState, arginfo_channel_get_connectivity_state, ZEND_ACC_PUBLIC)
    PHP_ME(Channel, watchConnectivityState, arginfo_channel_watch_connectivity_state, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry call_methods[] = {
    PHP_ME(Call, __construct, arginfo_call_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Call, startBatch, arginfo_call_start_batch, ZEND_ACC_PUBLIC)
    PHP_ME(Call, setCredentials, arginfo_call_set_credentials, ZEND_ACC_PUBLIC)
    PHP_ME(Call, cancel, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_ME(Call, getPeer, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(grpc_lite)
{
    zend_class_entry ce;
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    le_h2_stream = zend_register_list_destructors_ex(h2_stream_dtor, NULL, "grpc_lite_stream", module_number);

    memcpy(&grpc_channel_credentials_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_channel_credentials_handlers.offset = XtOffsetOf(grpc_lite_channel_credentials_obj, std);
    grpc_channel_credentials_handlers.free_obj = grpc_lite_channel_credentials_free_object;
    INIT_CLASS_ENTRY(ce, "Grpc\\ChannelCredentials", channel_credentials_methods);
    grpc_ce_channel_credentials = zend_register_internal_class(&ce);
    grpc_ce_channel_credentials->create_object = grpc_lite_channel_credentials_create_object;

    memcpy(&grpc_call_credentials_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_call_credentials_handlers.offset = XtOffsetOf(grpc_lite_call_credentials_obj, std);
    grpc_call_credentials_handlers.free_obj = grpc_lite_call_credentials_free_object;
    INIT_CLASS_ENTRY(ce, "Grpc\\CallCredentials", call_credentials_methods);
    grpc_ce_call_credentials = zend_register_internal_class(&ce);
    grpc_ce_call_credentials->create_object = grpc_lite_call_credentials_create_object;

    memcpy(&grpc_timeval_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_timeval_handlers.offset = XtOffsetOf(grpc_lite_timeval_obj, std);
    INIT_CLASS_ENTRY(ce, "Grpc\\Timeval", timeval_methods);
    grpc_ce_timeval = zend_register_internal_class(&ce);
    grpc_ce_timeval->create_object = grpc_lite_timeval_create_object;

    memcpy(&grpc_channel_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_channel_handlers.offset = XtOffsetOf(grpc_lite_channel_obj, std);
    grpc_channel_handlers.free_obj = grpc_lite_channel_free_object;
    INIT_CLASS_ENTRY(ce, "Grpc\\Channel", channel_methods);
    grpc_ce_channel = zend_register_internal_class(&ce);
    grpc_ce_channel->create_object = grpc_lite_channel_create_object;

    memcpy(&grpc_call_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_call_handlers.offset = XtOffsetOf(grpc_lite_call_obj, std);
    grpc_call_handlers.free_obj = grpc_lite_call_free_object;
    INIT_CLASS_ENTRY(ce, "Grpc\\Call", call_methods);
    grpc_ce_call = zend_register_internal_class(&ce);
    grpc_ce_call->create_object = grpc_lite_call_create_object;

    REGISTER_STRING_CONSTANT("Grpc\\VERSION", "0.1.0", CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_OK", GRPC_STATUS_OK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_CANCELLED", GRPC_STATUS_CANCELLED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_UNKNOWN", GRPC_STATUS_UNKNOWN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_INVALID_ARGUMENT", GRPC_STATUS_INVALID_ARGUMENT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_DEADLINE_EXCEEDED", GRPC_STATUS_DEADLINE_EXCEEDED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_NOT_FOUND", GRPC_STATUS_NOT_FOUND, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_ALREADY_EXISTS", GRPC_STATUS_ALREADY_EXISTS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_PERMISSION_DENIED", GRPC_STATUS_PERMISSION_DENIED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_RESOURCE_EXHAUSTED", GRPC_STATUS_RESOURCE_EXHAUSTED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_FAILED_PRECONDITION", GRPC_STATUS_FAILED_PRECONDITION, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_ABORTED", GRPC_STATUS_ABORTED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_OUT_OF_RANGE", GRPC_STATUS_OUT_OF_RANGE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_UNIMPLEMENTED", GRPC_STATUS_UNIMPLEMENTED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_INTERNAL", GRPC_STATUS_INTERNAL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_UNAVAILABLE", GRPC_STATUS_UNAVAILABLE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_DATA_LOSS", GRPC_STATUS_DATA_LOSS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\STATUS_UNAUTHENTICATED", GRPC_STATUS_UNAUTHENTICATED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CHANNEL_IDLE", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CHANNEL_CONNECTING", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CHANNEL_READY", 2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CHANNEL_TRANSIENT_FAILURE", 3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CHANNEL_FATAL_FAILURE", 4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_OK", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_NOT_ON_SERVER", 2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_NOT_ON_CLIENT", 3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_ALREADY_ACCEPTED", 4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_ALREADY_INVOKED", 5, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_NOT_INVOKED", 6, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_ALREADY_FINISHED", 7, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\CALL_ERROR_INVALID_FLAGS", 8, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\WRITE_BUFFER_HINT", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\WRITE_NO_COMPRESS", 2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_SEND_INITIAL_METADATA", GRPC_OP_SEND_INITIAL_METADATA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_SEND_MESSAGE", GRPC_OP_SEND_MESSAGE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_SEND_CLOSE_FROM_CLIENT", GRPC_OP_SEND_CLOSE_FROM_CLIENT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_SEND_STATUS_FROM_SERVER", 3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_RECV_INITIAL_METADATA", GRPC_OP_RECV_INITIAL_METADATA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_RECV_MESSAGE", GRPC_OP_RECV_MESSAGE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_RECV_STATUS_ON_CLIENT", GRPC_OP_RECV_STATUS_ON_CLIENT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("Grpc\\OP_RECV_CLOSE_ON_SERVER", 7, CONST_CS | CONST_PERSISTENT);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(grpc_lite)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "grpc_lite bridge", "enabled");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_unary, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_tls, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, max_receive_message_length, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, authority, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls_verify_name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_grpc_lite_stream_open, 0, 0, 5)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, use_tls, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, root_certs, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cert_chain, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, private_key, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, max_receive_message_length, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, authority, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tls_verify_name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_stream_next, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_stream_cancel, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_channel_close, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry grpc_lite_functions[] = {
    PHP_FE(grpc_lite_unary, arginfo_grpc_lite_unary)
    PHP_FE(grpc_lite_stream_open, arginfo_grpc_lite_stream_open)
    PHP_FE(grpc_lite_stream_next, arginfo_grpc_lite_stream_next)
    PHP_FE(grpc_lite_stream_cancel, arginfo_grpc_lite_stream_cancel)
    PHP_FE(grpc_lite_channel_close, arginfo_grpc_lite_channel_close)
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    PHP_FE(grpc_lite_multiplex_unary, arginfo_grpc_lite_multiplex_unary)
    PHP_FE(grpc_lite_bench_unary_batch, arginfo_grpc_lite_bench_unary_batch)
#endif
    PHP_FE_END
};

zend_module_entry grpc_module_entry = {
    STANDARD_MODULE_HEADER,
    "grpc",
    grpc_lite_functions,
    PHP_MINIT(grpc_lite),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(grpc_lite),
    "0.1.0",
    ZEND_MODULE_GLOBALS(grpc_lite),
    PHP_GINIT(grpc_lite),
    PHP_GSHUTDOWN(grpc_lite),
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_GRPC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(grpc)
#endif
