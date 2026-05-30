/* PHP class/object surface for the grpc extension. */

#include "surface.h"
#include "bridge.h"
#include "transport.h"

zend_class_entry *grpc_ce_channel;
zend_class_entry *grpc_ce_call;
zend_class_entry *grpc_ce_channel_credentials;
zend_class_entry *grpc_ce_call_credentials;
zend_class_entry *grpc_ce_timeval;

static zend_object_handlers grpc_channel_handlers;
static zend_object_handlers grpc_call_handlers;
static zend_object_handlers grpc_channel_credentials_handlers;
static zend_object_handlers grpc_call_credentials_handlers;
static zend_object_handlers grpc_timeval_handlers;

PHP_METHOD(Call, cancel);
PHP_METHOD(Call, getPeer);

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
    } else if ((type == GRPC_LITE_CREDENTIALS_SSL || type == GRPC_LITE_CREDENTIALS_DEFAULT) && PHP_GRPC_LITE_G(default_roots_pem) != NULL) {
        obj->root_certs = zend_string_copy(PHP_GRPC_LITE_G(default_roots_pem));
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

    if (PHP_GRPC_LITE_G(default_roots_pem) != NULL) {
        zend_string_release_ex(PHP_GRPC_LITE_G(default_roots_pem), true);
    }
    PHP_GRPC_LITE_G(default_roots_pem) = zend_string_init(ZSTR_VAL(roots), ZSTR_LEN(roots), 1);
}

PHP_METHOD(ChannelCredentials, isDefaultRootsPemSet)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_BOOL(PHP_GRPC_LITE_G(default_roots_pem) != NULL);
}

PHP_METHOD(ChannelCredentials, invalidateDefaultRootsPem)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (PHP_GRPC_LITE_G(default_roots_pem) != NULL) {
        zend_string_release_ex(PHP_GRPC_LITE_G(default_roots_pem), true);
        PHP_GRPC_LITE_G(default_roots_pem) = NULL;
    }
}

static zend_long grpc_lite_saturating_add(zend_long left, zend_long right)
{
    if (right > 0 && left > ZEND_LONG_MAX - right) {
        return ZEND_LONG_MAX;
    }
    if (right < 0 && left < ZEND_LONG_MIN - right) {
        return ZEND_LONG_MIN;
    }
    return left + right;
}

static zend_long grpc_lite_saturating_subtract(zend_long left, zend_long right)
{
    if (right == ZEND_LONG_MIN) {
        return left >= 0 ? ZEND_LONG_MAX : grpc_lite_saturating_add(left, ZEND_LONG_MAX);
    }
    return grpc_lite_saturating_add(left, -right);
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
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = grpc_lite_saturating_add(Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds, Z_GRPC_LITE_TIMEVAL_P(other)->microseconds);
}

PHP_METHOD(Timeval, subtract)
{
    zval *other;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(other, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    object_init_ex(return_value, grpc_ce_timeval);
    Z_GRPC_LITE_TIMEVAL_P(return_value)->microseconds = grpc_lite_saturating_subtract(Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds, Z_GRPC_LITE_TIMEVAL_P(other)->microseconds);
}

PHP_METHOD(Timeval, compare)
{
    zval *other;
    zend_long value;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(other, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();
    value = Z_GRPC_LITE_TIMEVAL_P(ZEND_THIS)->microseconds;
    if (value < Z_GRPC_LITE_TIMEVAL_P(other)->microseconds) {
        RETURN_LONG(-1);
    }
    if (value > Z_GRPC_LITE_TIMEVAL_P(other)->microseconds) {
        RETURN_LONG(1);
    }
    RETURN_LONG(0);
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
    obj->max_response_metadata_bytes = effective_max_response_metadata_bytes(-1, -1);
    obj->std.handlers = &grpc_channel_handlers;
    return &obj->std;
}

static void grpc_lite_channel_clear_fields(grpc_lite_channel_obj *obj)
{
    if (obj->target != NULL) {
        zend_string_release(obj->target);
        obj->target = NULL;
    }
    if (obj->host != NULL) {
        zend_string_release(obj->host);
        obj->host = NULL;
    }
    if (obj->authority != NULL) {
        zend_string_release(obj->authority);
        obj->authority = NULL;
    }
    if (obj->tls_verify_name != NULL) {
        zend_string_release(obj->tls_verify_name);
        obj->tls_verify_name = NULL;
    }
    if (obj->primary_user_agent != NULL) {
        zend_string_release(obj->primary_user_agent);
        obj->primary_user_agent = NULL;
    }
    if (obj->connection_key != NULL) {
        zend_string_release(obj->connection_key);
        obj->connection_key = NULL;
    }
    zval_ptr_dtor(&obj->credentials);
    ZVAL_UNDEF(&obj->credentials);
    obj->port = 443;
    obj->max_receive_message_length = 0;
    obj->max_response_metadata_bytes = effective_max_response_metadata_bytes(-1, -1);
    obj->initialized = false;
}

static void grpc_lite_channel_free_object(zend_object *object)
{
    grpc_lite_channel_obj *obj = grpc_lite_channel_fetch(object);
    grpc_lite_channel_clear_fields(obj);
    zend_object_std_dtor(&obj->std);
}

static int parse_target_port(const char *value, size_t len, zend_long *port)
{
    zend_long parsed = 0;
    size_t i;

    if (len == 0) {
        return FAILURE;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char) value[i];
        if (ch < '0' || ch > '9') {
            return FAILURE;
        }
        parsed = parsed * 10 + (zend_long) (ch - '0');
        if (parsed > 65535) {
            return FAILURE;
        }
    }
    if (parsed <= 0) {
        return FAILURE;
    }
    *port = parsed;
    return SUCCESS;
}

static int split_target_to_host_port(zend_string *target, zend_string **host, zend_long *port)
{
    const char *value = ZSTR_VAL(target);
    size_t len = ZSTR_LEN(target);
    const char *colon = memrchr(value, ':', len);
    size_t port_len;
    if (colon == NULL || colon == value || colon == value + len - 1) {
        *host = zend_string_copy(target);
        *port = 443;
        return SUCCESS;
    }
    port_len = (size_t) (value + len - colon - 1);
    if (parse_target_port(colon + 1, port_len, port) != SUCCESS) {
        return FAILURE;
    }
    *host = zend_string_init(value, (size_t) (colon - value), 0);
    return SUCCESS;
}

static void grpc_lite_sha256_hex(zend_string *value, char *hex)
{
    static const char chars[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t index;

    if (value == NULL || ZSTR_LEN(value) == 0) {
        hex[0] = '0';
        hex[1] = '\0';
        return;
    }

    SHA256((const unsigned char *) ZSTR_VAL(value), ZSTR_LEN(value), digest);
    for (index = 0; index < SHA256_DIGEST_LENGTH; index++) {
        hex[index * 2] = chars[digest[index] >> 4];
        hex[index * 2 + 1] = chars[digest[index] & 0x0f];
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
}

zend_string *grpc_lite_build_connection_key(const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, int credentials_type, zend_string *root_certs, zend_string *cert_chain, zend_string *private_key)
{
    smart_str canonical = {0};
    char root_certs_digest[SHA256_DIGEST_LENGTH * 2 + 1];
    char cert_chain_digest[SHA256_DIGEST_LENGTH * 2 + 1];
    char private_key_digest[SHA256_DIGEST_LENGTH * 2 + 1];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    static const char chars[] = "0123456789abcdef";
    size_t index;

    grpc_lite_sha256_hex(root_certs, root_certs_digest);
    grpc_lite_sha256_hex(cert_chain, cert_chain_digest);
    grpc_lite_sha256_hex(private_key, private_key_digest);

    smart_str_append_printf(&canonical, "%zu:%.*s|%ld|%zu:%.*s|%zu:%.*s|%d|%zu:%s|%zu:%s|%zu:%s",
        host_len,
        (int) host_len,
        host,
        (long) port,
        authority_len,
        (int) authority_len,
        authority != NULL ? authority : "",
        tls_verify_name_len,
        (int) tls_verify_name_len,
        tls_verify_name != NULL ? tls_verify_name : "",
        credentials_type,
        root_certs != NULL ? ZSTR_LEN(root_certs) : 0,
        root_certs_digest,
        cert_chain != NULL ? ZSTR_LEN(cert_chain) : 0,
        cert_chain_digest,
        private_key != NULL ? ZSTR_LEN(private_key) : 0,
        private_key_digest);
    smart_str_0(&canonical);
    if (canonical.s == NULL) {
        return NULL;
    }

    SHA256((const unsigned char *) ZSTR_VAL(canonical.s), ZSTR_LEN(canonical.s), digest);
    smart_str_free(&canonical);
    for (index = 0; index < SHA256_DIGEST_LENGTH; index++) {
        hex[index * 2] = chars[digest[index] >> 4];
        hex[index * 2 + 1] = chars[digest[index] & 0x0f];
    }
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return strpprintf(0, "sha256:%s", hex);
}

static zend_string *grpc_lite_build_channel_key(grpc_lite_channel_obj *channel)
{
    grpc_lite_channel_credentials_obj *credentials = Z_GRPC_LITE_CHANNEL_CREDENTIALS_P(&channel->credentials);
    return grpc_lite_build_connection_key(
        ZSTR_VAL(channel->host),
        ZSTR_LEN(channel->host),
        channel->port,
        channel->authority != NULL ? ZSTR_VAL(channel->authority) : NULL,
        channel->authority != NULL ? ZSTR_LEN(channel->authority) : 0,
        channel->tls_verify_name != NULL ? ZSTR_VAL(channel->tls_verify_name) : NULL,
        channel->tls_verify_name != NULL ? ZSTR_LEN(channel->tls_verify_name) : 0,
        credentials->type,
        credentials->root_certs,
        credentials->cert_chain,
        credentials->private_key);
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
    zval *max_metadata_size;
    zval *absolute_max_metadata_size;
    zend_long max_metadata_size_value = -1;
    zend_long absolute_max_metadata_size_value = -1;
    grpc_lite_channel_obj *obj = Z_GRPC_LITE_CHANNEL_P(ZEND_THIS);
    const char *error_message;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(target)
        Z_PARAM_ARRAY(opts)
    ZEND_PARSE_PARAMETERS_END();

    if (obj->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is already initialized", 0);
        RETURN_THROWS();
    }
    grpc_lite_channel_clear_fields(obj);

    credentials = zend_hash_str_find(Z_ARRVAL_P(opts), "credentials", sizeof("credentials") - 1);
    if (credentials == NULL || Z_TYPE_P(credentials) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(credentials), grpc_ce_channel_credentials)) {
        zend_throw_exception(NULL, "Channel options must include a ChannelCredentials instance", 0);
        RETURN_THROWS();
    }
    obj->target = zend_string_copy(target);
    if (split_target_to_host_port(target, &obj->host, &obj->port) != SUCCESS) {
        grpc_lite_channel_clear_fields(obj);
        zend_throw_exception(NULL, "invalid gRPC target port", 0);
        RETURN_THROWS();
    }
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
            grpc_lite_channel_clear_fields(obj);
            zend_throw_exception(NULL, "grpc.max_receive_message_length must be -1 or non-negative", 0);
            RETURN_THROWS();
        }
        obj->max_receive_message_length = Z_LVAL_P(max_receive_message_length);
    }
    max_metadata_size = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.max_metadata_size", sizeof("grpc.max_metadata_size") - 1);
    if (max_metadata_size != NULL) {
        if (Z_TYPE_P(max_metadata_size) != IS_LONG || Z_LVAL_P(max_metadata_size) < 0) {
            grpc_lite_channel_clear_fields(obj);
            zend_throw_exception(NULL, "grpc.max_metadata_size must be non-negative", 0);
            RETURN_THROWS();
        }
        max_metadata_size_value = Z_LVAL_P(max_metadata_size);
    }
    absolute_max_metadata_size = zend_hash_str_find(Z_ARRVAL_P(opts), "grpc.absolute_max_metadata_size", sizeof("grpc.absolute_max_metadata_size") - 1);
    if (absolute_max_metadata_size != NULL) {
        if (Z_TYPE_P(absolute_max_metadata_size) != IS_LONG || Z_LVAL_P(absolute_max_metadata_size) < 0) {
            grpc_lite_channel_clear_fields(obj);
            zend_throw_exception(NULL, "grpc.absolute_max_metadata_size must be non-negative", 0);
            RETURN_THROWS();
        }
        absolute_max_metadata_size_value = Z_LVAL_P(absolute_max_metadata_size);
    }
    if (max_metadata_size_value >= 0 && absolute_max_metadata_size_value >= 0 && absolute_max_metadata_size_value < max_metadata_size_value) {
        grpc_lite_channel_clear_fields(obj);
        zend_throw_exception(NULL, "grpc.absolute_max_metadata_size must be greater than or equal to grpc.max_metadata_size", 0);
        RETURN_THROWS();
    }
    obj->max_response_metadata_bytes = effective_max_response_metadata_bytes(max_metadata_size_value, absolute_max_metadata_size_value);
    error_message = validate_channel_inputs(
        ZSTR_VAL(target),
        ZSTR_LEN(target),
        ZSTR_VAL(obj->host),
        ZSTR_LEN(obj->host),
        obj->port,
        obj->authority != NULL ? ZSTR_VAL(obj->authority) : NULL,
        obj->authority != NULL ? ZSTR_LEN(obj->authority) : 0,
        obj->tls_verify_name != NULL ? ZSTR_VAL(obj->tls_verify_name) : NULL,
        obj->tls_verify_name != NULL ? ZSTR_LEN(obj->tls_verify_name) : 0);
    if (error_message != NULL) {
        grpc_lite_channel_clear_fields(obj);
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }
    obj->connection_key = grpc_lite_build_channel_key(obj);
    obj->initialized = true;
}

PHP_METHOD(Channel, getTarget)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (!Z_GRPC_LITE_CHANNEL_P(ZEND_THIS)->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
    RETURN_STR_COPY(Z_GRPC_LITE_CHANNEL_P(ZEND_THIS)->target);
}

PHP_METHOD(Channel, close)
{
    grpc_lite_channel_obj *obj = Z_GRPC_LITE_CHANNEL_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();
    if (!obj->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
    if (obj->connection_key == NULL) {
        zend_throw_exception(NULL, "Grpc\\Channel connection key is not initialized", 0);
        RETURN_THROWS();
    }
    if (PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        persistent_connection_entry *entry = zend_hash_find_ptr(&PHP_GRPC_LITE_G(persistent_connections), obj->connection_key);
        if (entry != NULL) {
            discard_persistent_connection(ZSTR_VAL(obj->connection_key), ZSTR_LEN(obj->connection_key), entry->connection);
        }
    }
}

PHP_METHOD(Channel, getConnectivityState)
{
    bool try_to_connect = false;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(try_to_connect)
    ZEND_PARSE_PARAMETERS_END();
    if (!Z_GRPC_LITE_CHANNEL_P(ZEND_THIS)->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
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
    if (!Z_GRPC_LITE_CHANNEL_P(ZEND_THIS)->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
    RETURN_FALSE;
}

static zend_object *grpc_lite_call_create_object(zend_class_entry *ce)
{
    grpc_lite_call_obj *obj = zend_object_alloc(sizeof(grpc_lite_call_obj), ce);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    ZVAL_UNDEF(&obj->channel);
    ZVAL_UNDEF(&obj->credentials);
    ZVAL_UNDEF(&obj->server_streaming_resource);
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
    zval_ptr_dtor(&obj->server_streaming_resource);
    zval_ptr_dtor(&obj->metadata);
    zval_ptr_dtor(&obj->initial_metadata);
    zval_ptr_dtor(&obj->trailing_metadata);
    zval_ptr_dtor(&obj->status);
    if (obj->method != NULL) zend_string_release(obj->method);
    if (obj->request_payload != NULL) zend_string_release(obj->request_payload);
    if (obj->unary_response_payload != NULL) zend_string_release(obj->unary_response_payload);
    zend_object_std_dtor(&obj->std);
}

PHP_METHOD(Call, __construct)
{
    zval *channel;
    zend_string *method;
    zval *deadline;
    grpc_lite_call_obj *obj = Z_GRPC_LITE_CALL_P(ZEND_THIS);
    grpc_lite_channel_obj *channel_obj;
    const char *error_message;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(channel, grpc_ce_channel)
        Z_PARAM_STR(method)
        Z_PARAM_OBJECT_OF_CLASS(deadline, grpc_ce_timeval)
    ZEND_PARSE_PARAMETERS_END();

    if (obj->initialized) {
        zend_throw_exception(NULL, "Grpc\\Call is already initialized", 0);
        RETURN_THROWS();
    }
    channel_obj = Z_GRPC_LITE_CHANNEL_P(channel);
    if (!channel_obj->initialized) {
        zend_throw_exception(NULL, "Grpc\\Channel is not initialized", 0);
        RETURN_THROWS();
    }
    error_message = validate_grpc_path(ZSTR_VAL(method), ZSTR_LEN(method));
    if (error_message != NULL) {
        zend_throw_exception(NULL, error_message, 0);
        RETURN_THROWS();
    }
    ZVAL_COPY(&obj->channel, channel);
    obj->method = zend_string_copy(method);
    obj->deadline_us = Z_GRPC_LITE_TIMEVAL_P(deadline)->microseconds;
    obj->initialized = true;
}

PHP_METHOD(Call, setCredentials)
{
    zval *credentials;
    grpc_lite_call_obj *obj = Z_GRPC_LITE_CALL_P(ZEND_THIS);

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(credentials, grpc_ce_call_credentials)
    ZEND_PARSE_PARAMETERS_END();

    if (!obj->initialized) {
        zend_throw_exception(NULL, "Grpc\\Call is not initialized", 0);
        RETURN_THROWS();
    }
    zval_ptr_dtor(&obj->credentials);
    ZVAL_COPY(&obj->credentials, credentials);
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

int grpc_lite_register_surface_classes(void)
{
    zend_class_entry ce;

    memcpy(&grpc_channel_credentials_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_channel_credentials_handlers.offset = XtOffsetOf(grpc_lite_channel_credentials_obj, std);
    grpc_channel_credentials_handlers.free_obj = grpc_lite_channel_credentials_free_object;
    grpc_channel_credentials_handlers.clone_obj = NULL;
    INIT_CLASS_ENTRY(ce, "Grpc\\ChannelCredentials", channel_credentials_methods);
    grpc_ce_channel_credentials = zend_register_internal_class(&ce);
    grpc_ce_channel_credentials->create_object = grpc_lite_channel_credentials_create_object;

    memcpy(&grpc_call_credentials_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_call_credentials_handlers.offset = XtOffsetOf(grpc_lite_call_credentials_obj, std);
    grpc_call_credentials_handlers.free_obj = grpc_lite_call_credentials_free_object;
    grpc_call_credentials_handlers.clone_obj = NULL;
    INIT_CLASS_ENTRY(ce, "Grpc\\CallCredentials", call_credentials_methods);
    grpc_ce_call_credentials = zend_register_internal_class(&ce);
    grpc_ce_call_credentials->create_object = grpc_lite_call_credentials_create_object;

    memcpy(&grpc_timeval_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_timeval_handlers.offset = XtOffsetOf(grpc_lite_timeval_obj, std);
    grpc_timeval_handlers.clone_obj = NULL;
    INIT_CLASS_ENTRY(ce, "Grpc\\Timeval", timeval_methods);
    grpc_ce_timeval = zend_register_internal_class(&ce);
    grpc_ce_timeval->create_object = grpc_lite_timeval_create_object;

    memcpy(&grpc_channel_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_channel_handlers.offset = XtOffsetOf(grpc_lite_channel_obj, std);
    grpc_channel_handlers.free_obj = grpc_lite_channel_free_object;
    grpc_channel_handlers.clone_obj = NULL;
    INIT_CLASS_ENTRY(ce, "Grpc\\Channel", channel_methods);
    grpc_ce_channel = zend_register_internal_class(&ce);
    grpc_ce_channel->create_object = grpc_lite_channel_create_object;

    memcpy(&grpc_call_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    grpc_call_handlers.offset = XtOffsetOf(grpc_lite_call_obj, std);
    grpc_call_handlers.free_obj = grpc_lite_call_free_object;
    grpc_call_handlers.clone_obj = NULL;
    INIT_CLASS_ENTRY(ce, "Grpc\\Call", call_methods);
    grpc_ce_call = zend_register_internal_class(&ce);
    grpc_ce_call->create_object = grpc_lite_call_create_object;

    return SUCCESS;
}
