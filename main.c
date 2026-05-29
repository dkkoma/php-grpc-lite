#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "surface.h"
#include "transport.h"

ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)

int le_server_streaming_call_state;

zend_class_entry *grpc_ce_channel;
zend_class_entry *grpc_ce_call;
zend_class_entry *grpc_ce_channel_credentials;
zend_class_entry *grpc_ce_call_credentials;
zend_class_entry *grpc_ce_timeval;
zend_object_handlers grpc_channel_handlers;
zend_object_handlers grpc_call_handlers;
zend_object_handlers grpc_channel_credentials_handlers;
zend_object_handlers grpc_call_credentials_handlers;
zend_object_handlers grpc_timeval_handlers;

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("grpc_lite.http2_stream_window_size", "8388608", PHP_INI_SYSTEM, OnUpdateLong, http2_stream_window_size, zend_grpc_lite_globals, grpc_lite_globals)
    STD_PHP_INI_ENTRY("grpc_lite.http2_connection_window_size", "8388608", PHP_INI_SYSTEM, OnUpdateLong, http2_connection_window_size, zend_grpc_lite_globals, grpc_lite_globals)
    STD_PHP_INI_ENTRY("grpc_lite.http2_max_frame_size", "16384", PHP_INI_SYSTEM, OnUpdateLong, http2_max_frame_size, zend_grpc_lite_globals, grpc_lite_globals)
    STD_PHP_INI_ENTRY("grpc_lite.http2_max_header_list_size", "65536", PHP_INI_SYSTEM, OnUpdateLong, http2_max_header_list_size, zend_grpc_lite_globals, grpc_lite_globals)
    STD_PHP_INI_ENTRY("grpc_lite.server_streaming_read_ahead_max_messages", "32", PHP_INI_ALL, OnUpdateLong, server_streaming_read_ahead_max_messages, zend_grpc_lite_globals, grpc_lite_globals)
    STD_PHP_INI_ENTRY("grpc_lite.server_streaming_read_ahead_max_bytes", "8388608", PHP_INI_ALL, OnUpdateLong, server_streaming_read_ahead_max_bytes, zend_grpc_lite_globals, grpc_lite_globals)
PHP_INI_END()

#ifndef PHP_GRPC_LITE_ENABLE_BENCH
const zend_function_entry grpc_lite_functions[] = {
    PHP_FE_END
};
#endif

PHP_GINIT_FUNCTION(grpc_lite)
{
#if defined(COMPILE_DL_GRPC) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    zend_hash_init(&grpc_lite_globals->persistent_connections, 8, NULL, NULL, 1);
    grpc_lite_globals->persistent_connections_initialized = true;
    grpc_lite_globals->default_roots_pem = NULL;
    grpc_lite_globals->http2_stream_window_size = 8 * 1024 * 1024;
    grpc_lite_globals->http2_connection_window_size = 8 * 1024 * 1024;
    grpc_lite_globals->http2_max_frame_size = GRPC_LITE_HTTP2_DEFAULT_MAX_FRAME_SIZE;
    grpc_lite_globals->http2_max_header_list_size = GRPC_LITE_HTTP2_DEFAULT_MAX_HEADER_LIST_SIZE;
    grpc_lite_globals->server_streaming_read_ahead_max_messages = 32;
    grpc_lite_globals->server_streaming_read_ahead_max_bytes = 8 * 1024 * 1024;
}

PHP_GSHUTDOWN_FUNCTION(grpc_lite)
{
    persistent_connection_entry *entry;

    if (!grpc_lite_globals->persistent_connections_initialized) {
        return;
    }

    ZEND_HASH_FOREACH_PTR(&grpc_lite_globals->persistent_connections, entry) {
        destroy_persistent_connection_entry(entry, true);
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(&grpc_lite_globals->persistent_connections);
    grpc_lite_globals->persistent_connections_initialized = false;
    if (grpc_lite_globals->default_roots_pem != NULL) {
        zend_string_release_ex(grpc_lite_globals->default_roots_pem, true);
        grpc_lite_globals->default_roots_pem = NULL;
    }
}

PHP_MINIT_FUNCTION(grpc_lite)
{
    zend_class_entry ce;
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    REGISTER_INI_ENTRIES();
    le_server_streaming_call_state = zend_register_list_destructors_ex(server_streaming_call_state_dtor, NULL, "grpc_lite_server_streaming_call_state", module_number);

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

PHP_MSHUTDOWN_FUNCTION(grpc_lite)
{
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(grpc_lite)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(grpc_lite)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "grpc_lite bridge", "enabled");
    php_info_print_table_row(2, "grpc_lite.http2_stream_window_size", INI_STR("grpc_lite.http2_stream_window_size"));
    php_info_print_table_row(2, "grpc_lite.http2_connection_window_size", INI_STR("grpc_lite.http2_connection_window_size"));
    php_info_print_table_row(2, "grpc_lite.http2_max_frame_size", INI_STR("grpc_lite.http2_max_frame_size"));
    php_info_print_table_row(2, "grpc_lite.http2_max_header_list_size", INI_STR("grpc_lite.http2_max_header_list_size"));
    php_info_print_table_row(2, "grpc_lite.server_streaming_read_ahead_max_messages", INI_STR("grpc_lite.server_streaming_read_ahead_max_messages"));
    php_info_print_table_row(2, "grpc_lite.server_streaming_read_ahead_max_bytes", INI_STR("grpc_lite.server_streaming_read_ahead_max_bytes"));
    php_info_print_table_end();
}

zend_module_entry grpc_module_entry = {
    STANDARD_MODULE_HEADER,
    "grpc",
    grpc_lite_functions,
    PHP_MINIT(grpc_lite),
    PHP_MSHUTDOWN(grpc_lite),
    PHP_RSHUTDOWN(grpc_lite),
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
