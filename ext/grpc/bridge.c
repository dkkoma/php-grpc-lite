/* Official grpc/grpc wrapper bridge. Included by main.c. */

#include "internal.h"

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

static zend_long grpc_lite_ceil_div_timeout(zend_long value, zend_long unit)
{
    return value / unit + (value % unit != 0 ? 1 : 0);
}

static void grpc_lite_append_timeout_metadata(zval *metadata, zend_long timeout_us)
{
    char timeout_buf[32];
    zval timeout_values;
    zend_long value;
    char unit;
    if (timeout_us <= 0) {
        return;
    }
    if (timeout_us <= 99999999L) {
        value = timeout_us;
        unit = 'u';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000L);
        unit = 'm';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 1000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 1000000L);
        unit = 'S';
    } else if (grpc_lite_ceil_div_timeout(timeout_us, 60000000L) <= 99999999L) {
        value = grpc_lite_ceil_div_timeout(timeout_us, 60000000L);
        unit = 'M';
    } else {
        value = grpc_lite_ceil_div_timeout(timeout_us, 3600000000L);
        if (value > 99999999L) {
            value = 99999999L;
        }
        unit = 'H';
    }
    snprintf(timeout_buf, sizeof(timeout_buf), "%ld%c", value, unit);
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
    value = zend_hash_str_find(Z_ARRVAL_P(result), "invalid_grpc_status", sizeof("invalid_grpc_status") - 1);
    if (value != NULL && zend_is_true(value)) return GRPC_STATUS_UNKNOWN;
    value = zend_hash_str_find(Z_ARRVAL_P(result), "grpc_status", sizeof("grpc_status") - 1);
    if (value == NULL || Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) < 0) {
        value = zend_hash_str_find(Z_ARRVAL_P(result), "http_status", sizeof("http_status") - 1);
        if (value != NULL && Z_TYPE_P(value) == IS_LONG && Z_LVAL_P(value) != 200) {
            switch (Z_LVAL_P(value)) {
                case 400: return GRPC_STATUS_INTERNAL;
                case 401: return GRPC_STATUS_UNAUTHENTICATED;
                case 403: return GRPC_STATUS_PERMISSION_DENIED;
                case 404: return GRPC_STATUS_UNIMPLEMENTED;
                case 429:
                case 502:
                case 503:
                case 504:
                    return GRPC_STATUS_UNAVAILABLE;
                default:
                    return Z_LVAL_P(value) < 0 ? GRPC_STATUS_UNAVAILABLE : GRPC_STATUS_UNKNOWN;
            }
        }
    }
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
    value = zend_hash_str_find(Z_ARRVAL_P(result), "stream_reset_seen", sizeof("stream_reset_seen") - 1);
    if (value != NULL && zend_is_true(value)) {
        zval *code = zend_hash_str_find(Z_ARRVAL_P(result), "stream_error_code", sizeof("stream_error_code") - 1);
        zend_long stream_error_code = code != NULL && Z_TYPE_P(code) == IS_LONG ? Z_LVAL_P(code) : -1;
        switch (stream_error_code) {
            case NGHTTP2_CANCEL:
                return GRPC_STATUS_CANCELLED;
            case NGHTTP2_REFUSED_STREAM:
                return GRPC_STATUS_UNAVAILABLE;
            case NGHTTP2_ENHANCE_YOUR_CALM:
                return GRPC_STATUS_RESOURCE_EXHAUSTED;
            case NGHTTP2_INADEQUATE_SECURITY:
                return GRPC_STATUS_PERMISSION_DENIED;
            case NGHTTP2_NO_ERROR:
            case NGHTTP2_PROTOCOL_ERROR:
            case NGHTTP2_INTERNAL_ERROR:
            case NGHTTP2_FLOW_CONTROL_ERROR:
            case NGHTTP2_SETTINGS_TIMEOUT:
            case NGHTTP2_STREAM_CLOSED:
            case NGHTTP2_FRAME_SIZE_ERROR:
            case NGHTTP2_COMPRESSION_ERROR:
            case NGHTTP2_CONNECT_ERROR:
                return GRPC_STATUS_INTERNAL;
            default:
                return GRPC_STATUS_UNKNOWN;
        }
    }
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
            value = zend_hash_str_find(Z_ARRVAL_P(result), "malformed_response_frame", sizeof("malformed_response_frame") - 1);
            if (value != NULL && zend_is_true(value)) {
                return zend_string_init("malformed gRPC response frame", sizeof("malformed gRPC response frame") - 1, 0);
            }
            value = zend_hash_str_find(Z_ARRVAL_P(result), "stream_reset_seen", sizeof("stream_reset_seen") - 1);
            if (value != NULL && zend_is_true(value)) {
                zval *stream_error_code = zend_hash_str_find(Z_ARRVAL_P(result), "stream_error_code", sizeof("stream_error_code") - 1);
                if (stream_error_code != NULL && Z_TYPE_P(stream_error_code) == IS_LONG) {
                    return strpprintf(0, "HTTP/2 stream reset: %ld", Z_LVAL_P(stream_error_code));
                }
                return zend_string_init("HTTP/2 stream reset", sizeof("HTTP/2 stream reset") - 1, 0);
            }
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
            value = zend_hash_str_find(Z_ARRVAL_P(result), "stream_reset_seen", sizeof("stream_reset_seen") - 1);
            if (value != NULL && zend_is_true(value)) {
                zval *stream_error_code = zend_hash_str_find(Z_ARRVAL_P(result), "stream_error_code", sizeof("stream_error_code") - 1);
                if (stream_error_code != NULL && Z_TYPE_P(stream_error_code) == IS_LONG) {
                    return strpprintf(0, "HTTP/2 stream reset: %ld", Z_LVAL_P(stream_error_code));
                }
                return zend_string_init("HTTP/2 stream reset", sizeof("HTTP/2 stream reset") - 1, 0);
            }
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
    bool protocol_failure;
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
    if (grpc_lite_unary_call_perform_on_channel(h2, ZSTR_VAL(call->method), ZSTR_LEN(call->method), ZSTR_VAL(framed), ZSTR_LEN(framed), &call->metadata, timeout_us, channel->max_receive_message_length, true, persistent_reused, &result) != SUCCESS) {
        zend_string_release(framed);
        zend_string_release(key);
        zval_ptr_dtor(&result);
        return FAILURE;
    }
    protocol_failure = grpc_lite_result_has_protocol_failure(&result);
    status_code = grpc_lite_status_from_result(&result);
    if (protocol_failure) {
        discard_persistent_channel(ZSTR_VAL(key), ZSTR_LEN(key), h2);
    } else if (!channel_usable(h2)) {
        remove_unusable_persistent_channel(ZSTR_VAL(key), ZSTR_LEN(key), h2);
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
    if (server_streaming_call_open_resource(
            ZSTR_VAL(key),
            ZSTR_LEN(key),
            ZSTR_VAL(channel->host),
            ZSTR_LEN(channel->host),
            channel->port,
            ZSTR_VAL(call->method),
            ZSTR_LEN(call->method),
            ZSTR_VAL(call->request),
            ZSTR_LEN(call->request),
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
            channel->authority != NULL ? ZSTR_VAL(channel->authority) : NULL,
            channel->authority != NULL ? ZSTR_LEN(channel->authority) : 0,
            channel->tls_verify_name != NULL ? ZSTR_VAL(channel->tls_verify_name) : NULL,
            channel->tls_verify_name != NULL ? ZSTR_LEN(channel->tls_verify_name) : 0,
            &call->stream) != SUCCESS) {
        zend_string_release(key);
        return FAILURE;
    }
    call->stream_opened = true;
    zend_string_release(key);
    return SUCCESS;
}

static int grpc_lite_server_streaming_next_for_call(grpc_lite_call_obj *call, zval *result)
{
    if (!call->stream_opened && grpc_lite_open_call_stream(call) != SUCCESS) {
        return FAILURE;
    }
    return server_streaming_call_next_resource(&call->stream, result);
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

    if (wants_status) {
        if (!call->unary_performed && !call->stream_opened) {
            if (grpc_lite_perform_call_unary(call) != SUCCESS) {
                RETURN_THROWS();
            }
        } else if (call->stream_opened && !call->status_ready) {
            zval result;
            do {
                ZVAL_UNDEF(&result);
                if (grpc_lite_server_streaming_next_for_call(call, &result) != SUCCESS) {
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
            if (grpc_lite_server_streaming_next_for_call(call, &result) != SUCCESS) {
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
    ZEND_PARSE_PARAMETERS_NONE();
    if (!call->initialized) {
        zend_throw_exception(NULL, "Grpc\\Call is not initialized", 0);
        RETURN_THROWS();
    }
    call->cancelled = true;
    if (call->stream_opened && Z_TYPE(call->stream) == IS_RESOURCE) {
        server_streaming_call_cancel_resource(&call->stream);
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
