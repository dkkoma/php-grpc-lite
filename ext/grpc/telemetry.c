/* Vendor-neutral telemetry record helpers. Included by main.c. */

#include "internal.h"

static bool grpc_lite_telemetry_enabled(void)
{
    return PHP_GRPC_LITE_G(telemetry_enabled) && !Z_ISUNDEF(PHP_GRPC_LITE_G(telemetry_handler));
}

static uint64_t unix_time_nanos(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t) ts.tv_sec * 1000000000ULL) + (uint64_t) ts.tv_nsec;
}

static void grpc_lite_telemetry_add_uint64(zval *array, const char *key, uint64_t value)
{
    if (value <= (uint64_t) ZEND_LONG_MAX) {
        add_assoc_long(array, key, (zend_long) value);
        return;
    }
    add_assoc_str(array, key, strpprintf(0, "%" PRIu64, value));
}

static void grpc_lite_telemetry_add_size(zval *array, const char *key, size_t value)
{
    if (value <= (size_t) ZEND_LONG_MAX) {
        add_assoc_long(array, key, (zend_long) value);
        return;
    }
    add_assoc_str(array, key, strpprintf(0, "%zu", value));
}

static bool grpc_lite_telemetry_read_metadata_first_string(zval *metadata, const char *key, size_t key_len, zend_string **value)
{
    zval *entry;
    zval *first;

    *value = NULL;
    if (metadata == NULL || Z_TYPE_P(metadata) != IS_ARRAY) {
        return false;
    }
    entry = zend_hash_str_find(Z_ARRVAL_P(metadata), key, key_len);
    if (entry == NULL) {
        return false;
    }
    if (Z_TYPE_P(entry) == IS_STRING) {
        *value = Z_STR_P(entry);
        return true;
    }
    if (Z_TYPE_P(entry) != IS_ARRAY) {
        return false;
    }
    first = zend_hash_index_find(Z_ARRVAL_P(entry), 0);
    if (first == NULL || Z_TYPE_P(first) != IS_STRING) {
        return false;
    }
    *value = Z_STR_P(first);
    return true;
}

static void grpc_lite_telemetry_split_method(const char *path, size_t path_len, zend_string **service, zend_string **method)
{
    const char *start = path;
    const char *slash;

    if (path_len > 0 && path[0] == '/') {
        start++;
        path_len--;
    }
    slash = memchr(start, '/', path_len);
    if (slash == NULL) {
        *service = zend_string_init(start, path_len, 0);
        *method = zend_string_copy(zend_empty_string);
        return;
    }
    *service = zend_string_init(start, (size_t) (slash - start), 0);
    *method = zend_string_init(slash + 1, (size_t) (start + path_len - slash - 1), 0);
}

static void grpc_lite_telemetry_clear_handler(void)
{
    if (!Z_ISUNDEF(PHP_GRPC_LITE_G(telemetry_handler))) {
        zval_ptr_dtor(&PHP_GRPC_LITE_G(telemetry_handler));
        ZVAL_UNDEF(&PHP_GRPC_LITE_G(telemetry_handler));
    }
}

static void grpc_lite_telemetry_call_handler(zval *record)
{
    zval retval;

    if (!grpc_lite_telemetry_enabled()) {
        return;
    }
    ZVAL_UNDEF(&retval);
    if (call_user_function(EG(function_table), NULL, &PHP_GRPC_LITE_G(telemetry_handler), &retval, 1, record) == SUCCESS) {
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
    }
    if (EG(exception)) {
        zend_clear_exception();
    }
}

static void grpc_lite_telemetry_build_unary_record(zval *record, const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status, uint64_t start_unix_nanos, uint64_t total_us, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused)
{
    zval timings;
    zval sizes;
    zval http2;
    zval connection_record;
    zend_string *service;
    zend_string *method;
    zend_string *traceparent;
    const char *detail_level = PHP_GRPC_LITE_G(telemetry_detail_level) != NULL ? PHP_GRPC_LITE_G(telemetry_detail_level) : "rpc";

    grpc_lite_telemetry_split_method(path, path_len, &service, &method);

    array_init(record);
    add_assoc_string(record, "kind", "unary");
    add_assoc_string(record, "rpc_system", "grpc");
    add_assoc_str(record, "rpc_service", zend_string_copy(service));
    add_assoc_str(record, "rpc_method", zend_string_copy(method));
    add_assoc_string(record, "network_protocol_name", "http");
    add_assoc_string(record, "network_protocol_version", "2");
    add_assoc_string(record, "backend", "http2");
    add_assoc_long(record, "grpc_status_code", status->code);
    add_assoc_long(record, "http_status_code", call->http_status);
    add_assoc_string(record, "detail_level", detail_level);
    add_assoc_str(record, "start_unix_nanos", strpprintf(0, "%" PRIu64, start_unix_nanos));
    grpc_lite_telemetry_add_uint64(record, "duration_us", total_us);
    if (status->details != NULL && ZSTR_LEN(status->details) > 0) {
        add_assoc_str(record, "grpc_status_details", zend_string_copy(status->details));
    }
    if (grpc_lite_telemetry_read_metadata_first_string(metadata, "traceparent", sizeof("traceparent") - 1, &traceparent)) {
        add_assoc_str(record, "traceparent", zend_string_copy(traceparent));
    }

    array_init(&timings);
    grpc_lite_telemetry_add_uint64(&timings, "setup_us", setup_us);
    grpc_lite_telemetry_add_uint64(&timings, "submit_us", submit_us);
    grpc_lite_telemetry_add_uint64(&timings, "initial_send_us", initial_send_us);
    grpc_lite_telemetry_add_uint64(&timings, "recv_loop_us", recv_loop_us);
    add_assoc_zval(record, "timings", &timings);

    array_init(&sizes);
    grpc_lite_telemetry_add_size(&sizes, "request_bytes", call->request_len);
    grpc_lite_telemetry_add_size(&sizes, "response_body_bytes", call->body.s ? ZSTR_LEN(call->body.s) : 0);
    grpc_lite_telemetry_add_size(&sizes, "bytes_sent", call->bytes_sent);
    grpc_lite_telemetry_add_size(&sizes, "bytes_received", call->bytes_received);
    add_assoc_zval(record, "sizes", &sizes);

    array_init(&http2);
    add_assoc_long(&http2, "sent_frames", call->sent_frames);
    add_assoc_long(&http2, "recv_frames", call->recv_frames);
    add_assoc_long(&http2, "stream_id", call->stream_id);
    add_assoc_long(&http2, "stream_error_code", call->stream_error_code);
    add_assoc_bool(&http2, "stream_reset_seen", call->stream_reset_seen);
    add_assoc_zval(record, "http2", &http2);

    array_init(&connection_record);
    add_assoc_bool(&connection_record, "reused", connection_reused);
    add_assoc_bool(&connection_record, "persistent_reused", persistent_reused);
    add_assoc_bool(&connection_record, "dead", connection->dead);
    add_assoc_bool(&connection_record, "draining", connection->draining);
    add_assoc_zval(record, "connection", &connection_record);

    zend_string_release(service);
    zend_string_release(method);
}

static void grpc_lite_telemetry_emit_unary(const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status, uint64_t start_unix_nanos, uint64_t total_us, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused)
{
    zval record;

    if (!grpc_lite_telemetry_enabled()) {
        return;
    }

    grpc_lite_telemetry_build_unary_record(&record, path, path_len, metadata, call, connection, status, start_unix_nanos, total_us, setup_us, submit_us, initial_send_us, recv_loop_us, connection_reused, persistent_reused);
    grpc_lite_telemetry_call_handler(&record);
    zval_ptr_dtor(&record);
}

static bool grpc_lite_telemetry_build_server_streaming_record(zval *record, server_streaming_call_state *state, grpc_lite_status_result *status)
{
    grpc_call *call;
    h2_connection *connection;
    zval timings;
    zval sizes;
    zval http2;
    zval connection_record;
    zend_string *service;
    zend_string *method;
    zend_string *traceparent;
    uint64_t total_us;
    const char *detail_level = PHP_GRPC_LITE_G(telemetry_detail_level) != NULL ? PHP_GRPC_LITE_G(telemetry_detail_level) : "rpc";

    if (state == NULL) {
        return false;
    }
    call = &state->call;
    connection = call->connection;
    if (state->path == NULL || connection == NULL) {
        return false;
    }

    grpc_lite_telemetry_split_method(ZSTR_VAL(state->path), ZSTR_LEN(state->path), &service, &method);
    total_us = state->total_started_us > 0 ? monotonic_us() - state->total_started_us : 0;

    array_init(record);
    add_assoc_string(record, "kind", "server_streaming");
    add_assoc_string(record, "rpc_system", "grpc");
    add_assoc_str(record, "rpc_service", zend_string_copy(service));
    add_assoc_str(record, "rpc_method", zend_string_copy(method));
    add_assoc_string(record, "network_protocol_name", "http");
    add_assoc_string(record, "network_protocol_version", "2");
    add_assoc_string(record, "backend", "http2");
    add_assoc_long(record, "grpc_status_code", status->code);
    add_assoc_long(record, "http_status_code", call->http_status);
    add_assoc_string(record, "detail_level", detail_level);
    add_assoc_str(record, "start_unix_nanos", strpprintf(0, "%" PRIu64, state->start_unix_nanos));
    grpc_lite_telemetry_add_uint64(record, "duration_us", total_us);
    grpc_lite_telemetry_add_uint64(record, "message_count", state->delivered_messages);
    if (status->details != NULL && ZSTR_LEN(status->details) > 0) {
        add_assoc_str(record, "grpc_status_details", zend_string_copy(status->details));
    }
    if (grpc_lite_telemetry_read_metadata_first_string(&state->metadata, "traceparent", sizeof("traceparent") - 1, &traceparent)) {
        add_assoc_str(record, "traceparent", zend_string_copy(traceparent));
    }

    array_init(&timings);
    grpc_lite_telemetry_add_uint64(&timings, "setup_us", state->setup_us);
    grpc_lite_telemetry_add_uint64(&timings, "submit_us", state->submit_us);
    grpc_lite_telemetry_add_uint64(&timings, "initial_send_us", state->initial_send_us);
    grpc_lite_telemetry_add_uint64(&timings, "recv_loop_us", state->recv_loop_us);
    add_assoc_zval(record, "timings", &timings);

    array_init(&sizes);
    grpc_lite_telemetry_add_size(&sizes, "request_bytes", call->request_len);
    grpc_lite_telemetry_add_uint64(&sizes, "response_body_bytes", state->delivered_payload_bytes);
    grpc_lite_telemetry_add_size(&sizes, "bytes_sent", call->bytes_sent);
    grpc_lite_telemetry_add_size(&sizes, "bytes_received", call->bytes_received);
    add_assoc_zval(record, "sizes", &sizes);

    array_init(&http2);
    add_assoc_long(&http2, "sent_frames", call->sent_frames);
    add_assoc_long(&http2, "recv_frames", call->recv_frames);
    add_assoc_long(&http2, "stream_id", call->stream_id);
    add_assoc_long(&http2, "stream_error_code", call->stream_error_code);
    add_assoc_bool(&http2, "stream_reset_seen", call->stream_reset_seen);
    add_assoc_zval(record, "http2", &http2);

    array_init(&connection_record);
    add_assoc_bool(&connection_record, "reused", state->persistent_reused);
    add_assoc_bool(&connection_record, "persistent_reused", state->persistent_reused);
    add_assoc_bool(&connection_record, "dead", connection->dead);
    add_assoc_bool(&connection_record, "draining", connection->draining);
    add_assoc_zval(record, "connection", &connection_record);

    zend_string_release(service);
    zend_string_release(method);
    return true;
}

static void grpc_lite_telemetry_emit_server_streaming(server_streaming_call_state *state, grpc_lite_status_result *status)
{
    zval record;

    if (state == NULL || state->telemetry_emitted || !grpc_lite_telemetry_enabled()) {
        if (state != NULL) {
            state->telemetry_emitted = true;
        }
        return;
    }
    if (!grpc_lite_telemetry_build_server_streaming_record(&record, state, status)) {
        state->telemetry_emitted = true;
        return;
    }

    grpc_lite_telemetry_call_handler(&record);
    zval_ptr_dtor(&record);
    state->telemetry_emitted = true;
}

#ifdef PHP_GRPC_LITE_ENABLE_BENCH
static zval *grpc_lite_telemetry_array_find(zval *array, const char *key)
{
    if (array == NULL || Z_TYPE_P(array) != IS_ARRAY) {
        return NULL;
    }
    return zend_hash_str_find(Z_ARRVAL_P(array), key, strlen(key));
}

static void grpc_lite_telemetry_diagnostic_add_long_from_record(zval *diagnostic_result, zval *record, const char *diagnostic_key, const char *record_key)
{
    zval *value = grpc_lite_telemetry_array_find(record, record_key);
    if (value == NULL) {
        add_assoc_long(diagnostic_result, diagnostic_key, 0);
    } else if (Z_TYPE_P(value) == IS_LONG) {
        add_assoc_long(diagnostic_result, diagnostic_key, Z_LVAL_P(value));
    } else if (Z_TYPE_P(value) == IS_TRUE || Z_TYPE_P(value) == IS_FALSE) {
        add_assoc_long(diagnostic_result, diagnostic_key, zend_is_true(value) ? 1 : 0);
    } else {
        add_assoc_long(diagnostic_result, diagnostic_key, 0);
    }
}

static void grpc_lite_telemetry_diagnostic_add_bool_from_record(zval *diagnostic_result, zval *record, const char *diagnostic_key, const char *record_key)
{
    zval *value = grpc_lite_telemetry_array_find(record, record_key);
    add_assoc_bool(diagnostic_result, diagnostic_key, value != NULL && zend_is_true(value));
}

static void grpc_lite_telemetry_add_common_diagnostic_fields(zval *diagnostic_result, zval *record)
{
    zval *timings = grpc_lite_telemetry_array_find(record, "timings");
    zval *sizes = grpc_lite_telemetry_array_find(record, "sizes");
    zval *http2 = grpc_lite_telemetry_array_find(record, "http2");
    zval *connection = grpc_lite_telemetry_array_find(record, "connection");

    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, record, "grpc_status", "grpc_status_code");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, record, "http_status", "http_status_code");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, record, "total_us", "duration_us");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, timings, "setup_us", "setup_us");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, timings, "submit_us", "submit_us");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, timings, "initial_send_us", "initial_send_us");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, timings, "recv_loop_us", "recv_loop_us");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, sizes, "body_bytes", "response_body_bytes");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, sizes, "bytes_sent", "bytes_sent");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, sizes, "bytes_received", "bytes_received");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, http2, "sent_frames", "sent_frames");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, http2, "recv_frames", "recv_frames");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, http2, "stream_error_code", "stream_error_code");
    grpc_lite_telemetry_diagnostic_add_bool_from_record(diagnostic_result, http2, "stream_reset_seen", "stream_reset_seen");
    grpc_lite_telemetry_diagnostic_add_bool_from_record(diagnostic_result, connection, "connection_reused", "reused");
    grpc_lite_telemetry_diagnostic_add_bool_from_record(diagnostic_result, connection, "persistent_reused", "persistent_reused");
    grpc_lite_telemetry_diagnostic_add_bool_from_record(diagnostic_result, connection, "connection_dead", "dead");
    grpc_lite_telemetry_diagnostic_add_bool_from_record(diagnostic_result, connection, "connection_draining", "draining");
    grpc_lite_telemetry_diagnostic_add_long_from_record(diagnostic_result, record, "message_count", "message_count");
}

static void grpc_lite_telemetry_add_unary_diagnostic_result(zval *diagnostic_result, const char *path, size_t path_len, zval *metadata, grpc_call *call, h2_connection *connection, grpc_lite_status_result *status, uint64_t start_unix_nanos, uint64_t total_us, uint64_t setup_us, uint64_t submit_us, uint64_t initial_send_us, uint64_t recv_loop_us, bool connection_reused, bool persistent_reused)
{
    zval record;

    array_init(diagnostic_result);
    grpc_lite_telemetry_build_unary_record(&record, path, path_len, metadata, call, connection, status, start_unix_nanos, total_us, setup_us, submit_us, initial_send_us, recv_loop_us, connection_reused, persistent_reused);
    grpc_lite_telemetry_add_common_diagnostic_fields(diagnostic_result, &record);
    add_assoc_zval(diagnostic_result, "telemetry", &record);
}

static void grpc_lite_telemetry_add_server_streaming_diagnostic_status(zval *diagnostic_result, server_streaming_call_state *state, grpc_lite_status_result *status)
{
    zval record;

    if (Z_TYPE_P(diagnostic_result) == IS_UNDEF) {
        array_init(diagnostic_result);
    }
    if (!grpc_lite_telemetry_build_server_streaming_record(&record, state, status)) {
        return;
    }
    grpc_lite_telemetry_add_common_diagnostic_fields(diagnostic_result, &record);
    add_assoc_zval(diagnostic_result, "telemetry", &record);
}
#endif

PHP_FUNCTION(grpc_lite_set_telemetry_handler)
{
    zval *handler;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    if (Z_TYPE_P(handler) != IS_NULL && !zend_is_callable(handler, 0, NULL)) {
        zend_argument_type_error(1, "must be a valid callback or null");
        RETURN_THROWS();
    }

    grpc_lite_telemetry_clear_handler();
    if (Z_TYPE_P(handler) != IS_NULL) {
        ZVAL_COPY(&PHP_GRPC_LITE_G(telemetry_handler), handler);
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_set_telemetry_handler, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 1)
ZEND_END_ARG_INFO()
