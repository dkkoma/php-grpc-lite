#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp2/nghttp2.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAKE_NV(NAME, VALUE) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE}
#define MAKE_NV_L(NAME, VALUE, VALUE_LEN) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, (VALUE_LEN), NGHTTP2_NV_FLAG_NONE}

typedef struct {
    int fd;
    int32_t stream_id;
    bool stream_closed;
    int grpc_status;
    uint32_t stream_error_code;
    int http_status;
    size_t bytes_sent;
    size_t bytes_received;
    size_t data_read_calls;
    size_t data_recv_calls;
    int last_session_error;
    int last_sent_frame_type;
    int last_recv_frame_type;
    int last_sent_frame_flags;
    int last_recv_frame_flags;
    int last_not_sent_frame_type;
    int last_not_sent_error;
    size_t sent_frames;
    size_t recv_frames;
    size_t not_sent_frames;
    smart_str body;
    uint8_t grpc_header[5];
    size_t grpc_header_len;
    const uint8_t *request;
    size_t request_len;
    size_t request_offset;
} poc_client;

static int connect_tcp(const char *host, zend_long port)
{
    char port_buf[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_buf, sizeof(port_buf), "%ld", port);

    if (getaddrinfo(host, port_buf, &hints, &result) != 0) {
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    while (total_written < length) {
        ssize_t written = send(client->fd, data + total_written, length - total_written, 0);
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t copied = 0;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    client->data_read_calls++;

    while (copied < length && client->request_offset < total_len) {
        if (client->request_offset < client->grpc_header_len) {
            size_t header_offset = client->request_offset;
            size_t remaining = client->grpc_header_len - header_offset;
            size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
            memcpy(buf + copied, client->grpc_header + header_offset, to_copy);
            copied += to_copy;
            client->request_offset += to_copy;
            continue;
        }

        size_t payload_offset = client->request_offset - client->grpc_header_len;
        size_t remaining = client->request_len - payload_offset;
        size_t to_copy = remaining < (length - copied) ? remaining : (length - copied);
        memcpy(buf + copied, client->request + payload_offset, to_copy);
        copied += to_copy;
        client->request_offset += to_copy;
    }

    if (client->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static void set_grpc_header(poc_client *client, size_t payload_len)
{
    client->grpc_header[0] = 0;
    client->grpc_header[1] = (uint8_t) ((payload_len >> 24) & 0xff);
    client->grpc_header[2] = (uint8_t) ((payload_len >> 16) & 0xff);
    client->grpc_header[3] = (uint8_t) ((payload_len >> 8) & 0xff);
    client->grpc_header[4] = (uint8_t) (payload_len & 0xff);
    client->grpc_header_len = 5;
}

static int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) flags;
    if (stream_id == client->stream_id && len > 0) {
        client->data_recv_calls++;
        smart_str_appendl(&client->body, (const char *) data, len);
    }
    return 0;
}

static int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) flags;
    if (frame->hd.stream_id != client->stream_id) {
        return 0;
    }
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->grpc_status = atoi(status_buf);
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->http_status = atoi(status_buf);
    }
    return 0;
}

static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) error_code;
    if (stream_id == client->stream_id) {
        client->stream_closed = true;
        client->stream_error_code = error_code;
    }
    return 0;
}

static int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->sent_frames++;
    client->last_sent_frame_type = frame->hd.type;
    client->last_sent_frame_flags = frame->hd.flags;
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->recv_frames++;
    client->last_recv_frame_type = frame->hd.type;
    client->last_recv_frame_flags = frame->hd.flags;
    return 0;
}

static int on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->not_sent_frames++;
    client->last_not_sent_frame_type = frame->hd.type;
    client->last_not_sent_error = lib_error_code;
    return 0;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000ULL) + ((uint64_t) ts.tv_nsec / 1000ULL);
}

PHP_FUNCTION(nghttp2_poc_unary)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zval *headers_zv = NULL;
    poc_client client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char authority[512];
    char header_storage[8][512];
    size_t header_storage_used = 0;
    int rv;
    char recv_buf[16384];

    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
    ZEND_PARSE_PARAMETERS_END();

    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;

    client.fd = connect_tcp(host, port);
    if (client.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);
    nghttp2_session_client_new(&session, callbacks, &client);
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
            zend_string *value_str;
            if (key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    client.stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, NULL);
    if (client.stream_id < 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "nghttp2_submit_request failed", 0);
        RETURN_THROWS();
    }

    rv = nghttp2_session_send(session);
    if (rv != 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
        RETURN_THROWS();
    }

    while (!client.stream_closed) {
        ssize_t nread = recv(client.fd, recv_buf, sizeof(recv_buf), 0);
        if (nread <= 0) {
            break;
        }
        client.bytes_received += (size_t) nread;
        rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
        if (rv < 0) {
            close(client.fd);
            nghttp2_session_del(session);
            nghttp2_session_callbacks_del(callbacks);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_mem_recv failed", 0);
            RETURN_THROWS();
        }
        rv = nghttp2_session_send(session);
        if (rv != 0) {
            close(client.fd);
            nghttp2_session_del(session);
            nghttp2_session_callbacks_del(callbacks);
            smart_str_free(&client.body);
            zend_throw_exception(NULL, "nghttp2_session_send failed", 0);
            RETURN_THROWS();
        }
        client.last_session_error = rv;
    }

    close(client.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);

    array_init(return_value);
    smart_str_0(&client.body);
    add_assoc_str(return_value, "body", client.body.s ? zend_string_copy(client.body.s) : zend_empty_string);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
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
    smart_str_free(&client.body);
}

PHP_FUNCTION(nghttp2_poc_unary_batch)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zend_long iterations = 0;
    zval *headers_zv = NULL;
    bool split_grpc_frame = false;
    poc_client client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    nghttp2_nv nva[16];
    size_t nvlen = 0;
    char authority[512];
    char header_storage[8][512];
    size_t header_storage_used = 0;
    int rv;
    char recv_buf[16384];
    zend_long ok = 0;
    zend_long failed = 0;
    uint64_t total_started;
    uint64_t total_elapsed;
    zval latencies;

    ZEND_PARSE_PARAMETERS_START(5, 7)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_LONG(iterations)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_BOOL(split_grpc_frame)
    ZEND_PARSE_PARAMETERS_END();

    if (iterations < 1) {
        zend_throw_exception(NULL, "iterations must be positive", 0);
        RETURN_THROWS();
    }

    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    if (split_grpc_frame) {
        set_grpc_header(&client, request_len);
    }

    client.fd = connect_tcp(host, port);
    if (client.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);
    nghttp2_session_client_new(&session, callbacks, &client);
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "nghttp2-poc/0.1");

    if (headers_zv != NULL) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers_zv), key, value) {
            zend_string *value_str;
            if (key == NULL || header_storage_used >= 8 || nvlen >= 16) {
                continue;
            }
            value_str = zval_get_string(value);
            snprintf(header_storage[header_storage_used], sizeof(header_storage[header_storage_used]), "%.*s", (int) ZSTR_LEN(value_str), ZSTR_VAL(value_str));
            nva[nvlen++] = (nghttp2_nv) {
                (uint8_t *) ZSTR_VAL(key),
                (uint8_t *) header_storage[header_storage_used],
                ZSTR_LEN(key),
                strlen(header_storage[header_storage_used]),
                NGHTTP2_NV_FLAG_NONE
            };
            header_storage_used++;
            zend_string_release(value_str);
        } ZEND_HASH_FOREACH_END();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = data_source_read_callback;
    array_init(&latencies);
    total_started = monotonic_us();

    for (zend_long i = 0; i < iterations; i++) {
        uint64_t started = monotonic_us();
        client.stream_closed = false;
        client.grpc_status = -1;
        client.http_status = -1;
        client.stream_error_code = 0;
        client.request_offset = 0;
        client.data_read_calls = 0;
        client.data_recv_calls = 0;
        smart_str_free(&client.body);
        memset(&client.body, 0, sizeof(client.body));

        client.stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, NULL);
        if (client.stream_id < 0) {
            failed++;
            break;
        }

        rv = nghttp2_session_send(session);
        if (rv != 0) {
            failed++;
            break;
        }

        while (!client.stream_closed) {
            ssize_t nread = recv(client.fd, recv_buf, sizeof(recv_buf), 0);
            if (nread <= 0) {
                failed++;
                break;
            }
            client.bytes_received += (size_t) nread;
            rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
            if (rv < 0) {
                failed++;
                break;
            }
            rv = nghttp2_session_send(session);
            if (rv != 0) {
                failed++;
                break;
            }
            client.last_session_error = rv;
        }

        add_next_index_long(&latencies, (zend_long) (monotonic_us() - started));
        if (client.stream_closed && client.grpc_status == 0 && client.http_status == 200) {
            ok++;
        } else {
            failed++;
            break;
        }
    }

    total_elapsed = monotonic_us() - total_started;

    close(client.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);

    array_init(return_value);
    add_assoc_long(return_value, "iterations", iterations);
    add_assoc_long(return_value, "ok", ok);
    add_assoc_long(return_value, "failed", failed);
    add_assoc_long(return_value, "total_us", (zend_long) total_elapsed);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
    add_assoc_long(return_value, "body_bytes", client.body.s ? ZSTR_LEN(client.body.s) : 0);
    add_assoc_bool(return_value, "split_grpc_frame", split_grpc_frame);
    add_assoc_long(return_value, "request_wire_bytes", client.grpc_header_len + client.request_len);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_zval(return_value, "latencies_us", &latencies);
    smart_str_free(&client.body);
}

PHP_MINIT_FUNCTION(nghttp2_poc)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(nghttp2_poc)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "nghttp2_poc support", "enabled");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_unary, 0, 4, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_nghttp2_poc_unary_batch, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, iterations, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, headers, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, split_grpc_frame, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

static const zend_function_entry nghttp2_poc_functions[] = {
    PHP_FE(nghttp2_poc_unary, arginfo_nghttp2_poc_unary)
    PHP_FE(nghttp2_poc_unary_batch, arginfo_nghttp2_poc_unary_batch)
    PHP_FE_END
};

zend_module_entry nghttp2_poc_module_entry = {
    STANDARD_MODULE_HEADER,
    "nghttp2_poc",
    nghttp2_poc_functions,
    PHP_MINIT(nghttp2_poc),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(nghttp2_poc),
    "0.1.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NGHTTP2_POC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(nghttp2_poc)
#endif
