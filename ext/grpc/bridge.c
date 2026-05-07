/* Official grpc/grpc wrapper bridge. Included by main.c. */

#include "internal.h"

static void grpc_lite_mark_call_failed(grpc_lite_call_obj *call, int code, zend_string *details);

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

static int grpc_lite_extract_unary_payload(zend_string *body_string, zend_string **payload)
{
    const unsigned char *body;
    size_t body_len;
    uint32_t payload_len;
    if (body_string == NULL) {
        *payload = NULL;
        return SUCCESS;
    }
    body = (const unsigned char *) ZSTR_VAL(body_string);
    body_len = ZSTR_LEN(body_string);
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

static int grpc_lite_perform_call_unary(grpc_lite_call_obj *call)
{
    grpc_lite_channel_obj *channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    zend_string *key = NULL;
    h2_connection *h2;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long timeout_us = grpc_lite_call_timeout_us(call);
    grpc_lite_unary_result result;
    zend_string *payload = NULL;
    int status_code;
    zend_string *details;

    if (call->request_payload == NULL) {
        zend_throw_exception(NULL, "Call has no request message", 0);
        return FAILURE;
    }
    if (grpc_lite_merge_call_credentials_metadata(call, channel) != SUCCESS) {
        return FAILURE;
    }
    grpc_lite_append_user_agent(channel, &call->metadata);
    grpc_lite_channel_key(channel, &key);
    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    h2 = get_persistent_connection(
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
        bool deadline_exceeded = (deadline_abs_us > 0 && monotonic_us() >= deadline_abs_us)
            || (error_message != NULL && strcmp(error_message, "HTTP/2 transport deadline exceeded") == 0);
        int code = deadline_exceeded ? GRPC_STATUS_DEADLINE_EXCEEDED : GRPC_STATUS_UNAVAILABLE;
        zend_string *details = zend_string_init(error_message != NULL ? error_message : "failed to open persistent connection", strlen(error_message != NULL ? error_message : "failed to open persistent connection"), 0);
        zval_ptr_dtor(&call->initial_metadata);
        array_init(&call->initial_metadata);
        zval_ptr_dtor(&call->trailing_metadata);
        array_init(&call->trailing_metadata);
        zval_ptr_dtor(&call->status);
        grpc_lite_make_status_object(&call->status, code, details, &call->trailing_metadata);
        call->initial_metadata_ready = true;
        call->status_ready = true;
        call->unary_performed = true;
        if (call->unary_response_payload != NULL) {
            zend_string_release(call->unary_response_payload);
            call->unary_response_payload = NULL;
        }
        zend_string_release(details);
        zend_string_release(key);
        return SUCCESS;
    }
    if (grpc_lite_unary_call_perform_on_connection(h2, ZSTR_VAL(call->method), ZSTR_LEN(call->method), ZSTR_VAL(call->request_payload), ZSTR_LEN(call->request_payload), &call->metadata, deadline_abs_us, channel->max_receive_message_length, channel->max_response_metadata_bytes, true, persistent_reused, &result) != SUCCESS) {
        zend_string_release(key);
        return FAILURE;
    }
    status_code = result.status.code;
    if (!connection_usable(h2)) {
        remove_unusable_persistent_connection(ZSTR_VAL(key), ZSTR_LEN(key), h2);
    }
    details = zend_string_copy(result.status.details);
    grpc_lite_copy_metadata(&call->initial_metadata, &result.initial_metadata);
    grpc_lite_copy_metadata(&call->trailing_metadata, &result.trailing_metadata);
    zval_ptr_dtor(&call->status);
    grpc_lite_make_status_object(&call->status, status_code, details, &call->trailing_metadata);
    call->initial_metadata_ready = true;
    call->status_ready = true;
    if (status_code == GRPC_STATUS_OK && result.body != NULL) {
        if (grpc_lite_extract_unary_payload(result.body, &payload) != SUCCESS) {
            status_code = GRPC_STATUS_INTERNAL;
            zend_string_release(details);
            details = zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
            zval_ptr_dtor(&call->status);
            grpc_lite_make_status_object(&call->status, status_code, details, &call->trailing_metadata);
        }
    } else if (status_code == GRPC_STATUS_OK) {
        status_code = GRPC_STATUS_INTERNAL;
        zend_string_release(details);
        details = zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
        zval_ptr_dtor(&call->status);
        grpc_lite_make_status_object(&call->status, status_code, details, &call->trailing_metadata);
    }
    if (payload != NULL) {
        if (call->unary_response_payload != NULL) {
            zend_string_release(call->unary_response_payload);
        }
        call->unary_response_payload = payload;
    } else {
        if (call->unary_response_payload != NULL) {
            zend_string_release(call->unary_response_payload);
            call->unary_response_payload = NULL;
        }
    }
    call->unary_performed = true;
    zend_string_release(details);
    zend_string_release(key);
    grpc_lite_unary_result_dtor(&result);
    return SUCCESS;
}

static int grpc_lite_open_call_stream(grpc_lite_call_obj *call)
{
    grpc_lite_channel_obj *channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    zend_string *key = NULL;
    zend_long timeout_us = grpc_lite_call_timeout_us(call);
    grpc_lite_status_result setup_failure = {0};

    if (call->server_streaming_opened) {
        return SUCCESS;
    }
    if (call->request_payload == NULL) {
        zend_throw_exception(NULL, "Call has no request message", 0);
        return FAILURE;
    }
    if (grpc_lite_merge_call_credentials_metadata(call, channel) != SUCCESS) {
        return FAILURE;
    }
    grpc_lite_append_user_agent(channel, &call->metadata);
    grpc_lite_channel_key(channel, &key);

    zval_ptr_dtor(&call->server_streaming_resource);
    if (server_streaming_call_open_resource(
            ZSTR_VAL(key),
            ZSTR_LEN(key),
            ZSTR_VAL(channel->host),
            ZSTR_LEN(channel->host),
            channel->port,
            ZSTR_VAL(call->method),
            ZSTR_LEN(call->method),
            ZSTR_VAL(call->request_payload),
            ZSTR_LEN(call->request_payload),
            &call->metadata,
            timeout_us,
            credentials->type != GRPC_LITE_CREDENTIALS_INSECURE,
            credentials->root_certs != NULL ? ZSTR_VAL(credentials->root_certs) : NULL,
            credentials->root_certs != NULL ? ZSTR_LEN(credentials->root_certs) : 0,
            credentials->cert_chain != NULL ? ZSTR_VAL(credentials->cert_chain) : NULL,
            credentials->cert_chain != NULL ? ZSTR_LEN(credentials->cert_chain) : 0,
            credentials->private_key != NULL ? ZSTR_VAL(credentials->private_key) : NULL,
            credentials->private_key != NULL ? ZSTR_LEN(credentials->private_key) : 0,
            channel->max_receive_message_length,
            channel->max_response_metadata_bytes,
            channel->authority != NULL ? ZSTR_VAL(channel->authority) : NULL,
            channel->authority != NULL ? ZSTR_LEN(channel->authority) : 0,
            channel->tls_verify_name != NULL ? ZSTR_VAL(channel->tls_verify_name) : NULL,
            channel->tls_verify_name != NULL ? ZSTR_LEN(channel->tls_verify_name) : 0,
            &call->server_streaming_resource,
            &setup_failure) != SUCCESS) {
        zend_string_release(key);
        return FAILURE;
    }
    call->server_streaming_opened = true;
    if (setup_failure.details != NULL) {
        grpc_lite_mark_call_failed(call, setup_failure.code, setup_failure.details);
        zend_string_release(setup_failure.details);
    }
    zend_string_release(key);
    return SUCCESS;
}

static int grpc_lite_server_streaming_next_for_call(grpc_lite_call_obj *call, grpc_lite_streaming_next_result *result)
{
    if (!call->server_streaming_opened && grpc_lite_open_call_stream(call) != SUCCESS) {
        return FAILURE;
    }
    if (call->status_ready) {
        zval *code_zv;
        zval *details_zv;
        memset(result, 0, sizeof(*result));
        result->done = true;
        code_zv = zend_read_property(Z_OBJCE(call->status), Z_OBJ(call->status), "code", sizeof("code") - 1, 0, NULL);
        details_zv = zend_read_property(Z_OBJCE(call->status), Z_OBJ(call->status), "details", sizeof("details") - 1, 0, NULL);
        result->status.code = (code_zv != NULL && Z_TYPE_P(code_zv) == IS_LONG) ? (int) Z_LVAL_P(code_zv) : GRPC_STATUS_UNKNOWN;
        result->status.details = (details_zv != NULL && Z_TYPE_P(details_zv) == IS_STRING) ? zend_string_copy(Z_STR_P(details_zv)) : zend_string_copy(zend_empty_string);
        grpc_lite_copy_metadata(&result->initial_metadata, &call->initial_metadata);
        grpc_lite_copy_metadata(&result->trailing_metadata, &call->trailing_metadata);
        return SUCCESS;
    }
    return server_streaming_call_next_resource(&call->server_streaming_resource, result);
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

static void grpc_lite_mark_call_failed(grpc_lite_call_obj *call, int code, zend_string *details)
{
    zval_ptr_dtor(&call->initial_metadata);
    array_init(&call->initial_metadata);
    zval_ptr_dtor(&call->trailing_metadata);
    array_init(&call->trailing_metadata);
    zval_ptr_dtor(&call->status);
    grpc_lite_make_status_object(&call->status, code, details, &call->trailing_metadata);
    call->initial_metadata_ready = true;
    call->status_ready = true;
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
        if (call->request_payload != NULL) {
            zend_string_release(call->request_payload);
        }
        call->request_payload = zend_string_copy(Z_STR_P(payload));
        if (call->unary_response_payload != NULL) {
            zend_string_release(call->unary_response_payload);
            call->unary_response_payload = NULL;
        }
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

    if (!call->initialized) {
        zend_throw_exception(NULL, "Grpc\\Call is not initialized", 0);
        RETURN_THROWS();
    }

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

    if (call->status_ready) {
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
        if (!call->unary_performed && !call->server_streaming_opened) {
            if (grpc_lite_perform_call_unary(call) != SUCCESS) {
                RETURN_THROWS();
            }
        } else if (call->server_streaming_opened && !call->status_ready) {
            grpc_lite_streaming_next_result result;
            do {
                if (grpc_lite_server_streaming_next_for_call(call, &result) != SUCCESS) {
                    RETURN_THROWS();
                }
                if (result.done) {
                    if (!call->initial_metadata_ready) {
                        grpc_lite_copy_metadata(&call->initial_metadata, &result.initial_metadata);
                        call->initial_metadata_ready = true;
                    }
                    grpc_lite_copy_metadata(&call->trailing_metadata, &result.trailing_metadata);
                    zval_ptr_dtor(&call->status);
                    grpc_lite_make_status_object(&call->status, result.status.code, result.status.details, &call->trailing_metadata);
                    call->status_ready = true;
                }
                grpc_lite_streaming_next_result_dtor(&result);
            } while (!call->status_ready);
        }
    } else if (wants_message && !call->unary_performed) {
        if (call->server_streaming_opened || !zend_hash_index_exists(Z_ARRVAL_P(ops), GRPC_OP_RECV_STATUS_ON_CLIENT)) {
            grpc_lite_streaming_next_result result;
            if (grpc_lite_server_streaming_next_for_call(call, &result) != SUCCESS) {
                RETURN_THROWS();
            }
            if (result.done) {
                if (!call->initial_metadata_ready) {
                    grpc_lite_copy_metadata(&call->initial_metadata, &result.initial_metadata);
                    call->initial_metadata_ready = true;
                }
                grpc_lite_copy_metadata(&call->trailing_metadata, &result.trailing_metadata);
                zval_ptr_dtor(&call->status);
                grpc_lite_make_status_object(&call->status, result.status.code, result.status.details, &call->trailing_metadata);
                call->status_ready = true;
                grpc_lite_add_event_message(return_value, NULL);
            } else {
                if (!call->initial_metadata_ready) {
                    grpc_lite_copy_metadata(&call->initial_metadata, &result.initial_metadata);
                    call->initial_metadata_ready = true;
                }
                grpc_lite_add_event_message(return_value, result.payload);
            }
            grpc_lite_streaming_next_result_dtor(&result);
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
        grpc_lite_add_event_message(return_value, call->unary_response_payload);
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
    ZEND_PARSE_PARAMETERS_NONE();
    if (!call->initialized) {
        zend_throw_exception(NULL, "Grpc\\Call is not initialized", 0);
        RETURN_THROWS();
    }
    call->cancelled = true;
    if (call->server_streaming_opened && Z_TYPE(call->server_streaming_resource) == IS_RESOURCE) {
        server_streaming_call_cancel_resource(&call->server_streaming_resource);
    }
}

PHP_METHOD(Call, getPeer)
{
    grpc_lite_call_obj *call = Z_GRPC_LITE_CALL_P(ZEND_THIS);
    grpc_lite_channel_obj *channel;
    ZEND_PARSE_PARAMETERS_NONE();
    if (!call->initialized || Z_TYPE(call->channel) != IS_OBJECT) {
        zend_throw_exception(NULL, "Grpc\\Call is not initialized", 0);
        RETURN_THROWS();
    }
    channel = Z_GRPC_LITE_CHANNEL_P(&call->channel);
    if (!channel->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
    RETURN_STR_COPY(channel->target);
}

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


static const zend_function_entry call_methods[] = {
    PHP_ME(Call, __construct, arginfo_call_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Call, startBatch, arginfo_call_start_batch, ZEND_ACC_PUBLIC)
    PHP_ME(Call, setCredentials, arginfo_call_set_credentials, ZEND_ACC_PUBLIC)
    PHP_ME(Call, cancel, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_ME(Call, getPeer, arginfo_no_args, ZEND_ACC_PUBLIC)
    PHP_FE_END
};
