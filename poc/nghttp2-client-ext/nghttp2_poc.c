#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <php.h>
#include <ext/standard/info.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>
#include <nghttp2/nghttp2.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
    size_t data_read_length_calls;
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
    size_t data_frames_sent;
    size_t data_bytes_sent;
    size_t window_update_frames_recv;
    size_t connection_window_update_frames_recv;
    size_t stream_window_update_frames_recv;
    size_t connection_window_update_increment_recv;
    size_t stream_window_update_increment_recv;
    size_t flow_control_pauses;
    size_t send_callback_calls;
    size_t send_data_callback_calls;
    size_t write_syscalls;
    size_t send_wouldblock_calls;
    size_t recv_wouldblock_calls;
    size_t poll_calls;
    size_t poll_timeouts;
    size_t poll_errors;
    uint64_t max_write_syscall_us;
    size_t max_send_callback_len;
    size_t max_data_frame_len;
    size_t min_data_frame_len;
    size_t max_read_len;
    size_t min_read_len;
    uint32_t data_frame_size_cap;
    int32_t min_session_remote_window;
    int32_t min_stream_remote_window;
    uint32_t remote_max_frame_size;
    bool no_copy;
    bool poll_loop;
    bool last_send_wouldblock;
    uint64_t call_started_us;
    uint64_t first_data_sent_us;
    uint64_t last_data_sent_us;
    uint64_t first_window_update_us;
    uint64_t last_window_update_us;
    uint64_t first_flow_control_pause_us;
    uint64_t first_response_header_us;
    uint64_t stream_closed_us;
    size_t call_window_update_frames_recv;
    size_t call_connection_window_update_frames_recv;
    size_t call_stream_window_update_frames_recv;
    size_t call_connection_window_update_increment_recv;
    size_t call_stream_window_update_increment_recv;
    size_t call_data_read_length_calls;
    size_t call_flow_control_pauses;
    uint64_t call_max_write_syscall_us;
    int32_t call_min_session_remote_window;
    int32_t call_min_stream_remote_window;
    zend_long server_handler_ns;
    zend_long server_payload_alloc_ns;
    zend_long server_payload_bytes;
    zend_long server_request_payload_bytes;
    zend_long server_stats_handler_start_ns;
    zend_long server_stats_handler_end_ns;
    zend_long server_stats_in_payload_ns;
    zend_long server_stats_out_header_ns;
    zend_long server_stats_out_payload_ns;
    zend_long server_stats_out_payload_bytes;
    zend_long server_stats_out_payload_wire_bytes;
    zend_long server_stats_out_payload_compressed_bytes;
    smart_str body;
    uint8_t grpc_header[5];
    size_t grpc_header_len;
    const uint8_t *request;
    size_t request_len;
    size_t request_offset;
    size_t pending_data_len;
} poc_client;

static uint64_t monotonic_us(void);
static zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
static void record_data_sent(poc_client *client);

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

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    client->send_callback_calls++;
    if (length > client->max_send_callback_len) {
        client->max_send_callback_len = length;
    }

    if (client->poll_loop) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = send(client->fd, data, length, 0);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                client->last_send_wouldblock = true;
                client->send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            client->last_send_wouldblock = true;
            client->send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        client->write_syscalls++;
        client->bytes_sent += (size_t) written;
        return written;
    }

    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = send(client->fd, data + total_written, length - total_written, 0);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        client->write_syscalls++;
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static size_t remaining_request_bytes(poc_client *client)
{
    size_t total_len = client->grpc_header_len + client->request_len;
    if (client->request_offset >= total_len) {
        return 0;
    }
    return total_len - client->request_offset;
}

static size_t copy_request_bytes(poc_client *client, uint8_t *buf, size_t length)
{
    size_t copied = 0;
    size_t total_len = client->grpc_header_len + client->request_len;

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

    return copied;
}

static ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t remaining = remaining_request_bytes(client);
    size_t to_send = remaining < length ? remaining : length;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    client->data_read_calls++;

    if (client->no_copy && to_send > 0) {
        client->pending_data_len = to_send;
        *data_flags = NGHTTP2_DATA_FLAG_NO_COPY;
        if (client->request_offset + to_send >= total_len) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return (ssize_t) to_send;
    }

    size_t copied = copy_request_bytes(client, buf, to_send);
    if (client->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static ssize_t data_source_read_length_callback(nghttp2_session *session, uint8_t frame_type, int32_t stream_id, int32_t session_remote_window_size, int32_t stream_remote_window_size, uint32_t remote_max_frame_size, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    size_t allowed = (size_t) session_remote_window_size;
    (void) session;
    (void) frame_type;
    (void) stream_id;

    if (stream_remote_window_size < session_remote_window_size) {
        allowed = (size_t) stream_remote_window_size;
    }
    if (remote_max_frame_size < allowed) {
        allowed = remote_max_frame_size;
    }
    if (client->data_frame_size_cap > 0 && client->data_frame_size_cap < allowed) {
        allowed = client->data_frame_size_cap;
    }
    if (allowed == 0) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->flow_control_pauses++;
        client->call_flow_control_pauses++;
        if (elapsed > 0 && client->first_flow_control_pause_us == 0) {
            client->first_flow_control_pause_us = elapsed;
        }
        return NGHTTP2_ERR_PAUSE;
    }

    client->data_read_length_calls++;
    client->call_data_read_length_calls++;
    client->remote_max_frame_size = remote_max_frame_size;
    if (client->min_session_remote_window == 0 || session_remote_window_size < client->min_session_remote_window) {
        client->min_session_remote_window = session_remote_window_size;
    }
    if (client->min_stream_remote_window == 0 || stream_remote_window_size < client->min_stream_remote_window) {
        client->min_stream_remote_window = stream_remote_window_size;
    }
    if (client->call_min_session_remote_window == 0 || session_remote_window_size < client->call_min_session_remote_window) {
        client->call_min_session_remote_window = session_remote_window_size;
    }
    if (client->call_min_stream_remote_window == 0 || stream_remote_window_size < client->call_min_stream_remote_window) {
        client->call_min_stream_remote_window = stream_remote_window_size;
    }
    if (allowed > client->max_read_len) {
        client->max_read_len = allowed;
    }
    if (client->min_read_len == 0 || allowed < client->min_read_len) {
        client->min_read_len = allowed;
    }

    return (ssize_t) allowed;
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

static int write_all(poc_client *client, const uint8_t *data, size_t length)
{
    size_t total_written = 0;
    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = send(client->fd, data + total_written, length - total_written, 0);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        client->write_syscalls++;
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return 0;
}

static size_t fill_request_iov(poc_client *client, struct iovec *iov, size_t iov_offset, size_t length, size_t *filled_len)
{
    size_t filled = 0;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t request_offset = client->request_offset;

    while (filled < length && request_offset < total_len && iov_offset < 4) {
        if (request_offset < client->grpc_header_len) {
            size_t header_offset = request_offset;
            size_t remaining = client->grpc_header_len - header_offset;
            size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
            iov[iov_offset].iov_base = client->grpc_header + header_offset;
            iov[iov_offset].iov_len = to_write;
            iov_offset++;
            filled += to_write;
            request_offset += to_write;
            continue;
        }

        size_t payload_offset = request_offset - client->grpc_header_len;
        size_t remaining = client->request_len - payload_offset;
        size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
        iov[iov_offset].iov_base = (void *) (client->request + payload_offset);
        iov[iov_offset].iov_len = to_write;
        iov_offset++;
        filled += to_write;
        request_offset += to_write;
    }

    *filled_len = filled;
    return iov_offset;
}

static int write_data_frame(poc_client *client, const uint8_t *framehd, size_t length)
{
    struct iovec iov[4];
    size_t iovcnt = 1;
    size_t filled_len = 0;
    size_t total_len = 9 + length;
    size_t total_written = 0;

    iov[0].iov_base = (void *) framehd;
    iov[0].iov_len = 9;
    iovcnt = fill_request_iov(client, iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        return -1;
    }

    while (total_written < total_len) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(client->fd, iov, (int) iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->max_write_syscall_us) {
            client->max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->call_max_write_syscall_us) {
            client->call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        client->write_syscalls++;
        total_written += (size_t) written;

        size_t consumed = (size_t) written;
        for (size_t i = 0; i < iovcnt && consumed > 0; i++) {
            if (consumed >= iov[i].iov_len) {
                consumed -= iov[i].iov_len;
                iov[i].iov_base = (uint8_t *) iov[i].iov_base + iov[i].iov_len;
                iov[i].iov_len = 0;
                continue;
            }
            iov[i].iov_base = (uint8_t *) iov[i].iov_base + consumed;
            iov[i].iov_len -= consumed;
            consumed = 0;
        }
        while (iovcnt > 0 && iov[0].iov_len == 0) {
            memmove(&iov[0], &iov[1], sizeof(struct iovec) * (iovcnt - 1));
            iovcnt--;
        }
    }

    client->request_offset += length;
    client->bytes_sent += total_len;
    return 0;
}

static int send_data_callback(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    (void) frame;
    (void) source;

    client->send_data_callback_calls++;
    client->data_frames_sent++;
    client->data_bytes_sent += length;
    if (length > client->max_data_frame_len) {
        client->max_data_frame_len = length;
    }
    if (client->min_data_frame_len == 0 || length < client->min_data_frame_len) {
        client->min_data_frame_len = length;
    }

    if (write_data_frame(client, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    record_data_sent(client);
    client->pending_data_len = 0;

    return 0;
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
        if (client->first_response_header_us == 0) {
            client->first_response_header_us = monotonic_us() - client->call_started_us;
        }
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->http_status = atoi(status_buf);
    } else if (namelen == sizeof("x-bench-server-handler-ns") - 1 && memcmp(name, "x-bench-server-handler-ns", namelen) == 0) {
        client->server_handler_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-alloc-ns") - 1 && memcmp(name, "x-bench-server-payload-alloc-ns", namelen) == 0) {
        client->server_payload_alloc_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-bytes") - 1 && memcmp(name, "x-bench-server-payload-bytes", namelen) == 0) {
        client->server_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-request-payload-bytes") - 1 && memcmp(name, "x-bench-server-request-payload-bytes", namelen) == 0) {
        client->server_request_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-start-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-start-ns", namelen) == 0) {
        client->server_stats_handler_start_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-end-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-end-ns", namelen) == 0) {
        client->server_stats_handler_end_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-in-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-in-payload-ns", namelen) == 0) {
        client->server_stats_in_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-header-ns") - 1 && memcmp(name, "x-bench-server-stats-out-header-ns", namelen) == 0) {
        client->server_stats_out_header_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-out-payload-ns", namelen) == 0) {
        client->server_stats_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-bytes", namelen) == 0) {
        client->server_stats_out_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-wire-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-wire-bytes", namelen) == 0) {
        client->server_stats_out_payload_wire_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-compressed-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-compressed-bytes", namelen) == 0) {
        client->server_stats_out_payload_compressed_bytes = header_value_to_long(value, valuelen);
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
        client->stream_closed_us = monotonic_us() - client->call_started_us;
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
    if (frame->hd.type == NGHTTP2_DATA && !client->no_copy) {
        client->data_frames_sent++;
        client->data_bytes_sent += frame->hd.length;
        record_data_sent(client);
        if (frame->hd.length > client->max_data_frame_len) {
            client->max_data_frame_len = frame->hd.length;
        }
        if (client->min_data_frame_len == 0 || frame->hd.length < client->min_data_frame_len) {
            client->min_data_frame_len = frame->hd.length;
        }
    }
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    poc_client *client = (poc_client *) user_data;
    (void) session;
    client->recv_frames++;
    client->last_recv_frame_type = frame->hd.type;
    client->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = client->call_started_us == 0 ? 0 : monotonic_us() - client->call_started_us;
        client->window_update_frames_recv++;
        client->call_window_update_frames_recv++;
        if (frame->hd.stream_id == 0) {
            client->connection_window_update_frames_recv++;
            client->call_connection_window_update_frames_recv++;
            client->connection_window_update_increment_recv += frame->window_update.window_size_increment;
            client->call_connection_window_update_increment_recv += frame->window_update.window_size_increment;
        } else {
            client->stream_window_update_frames_recv++;
            client->call_stream_window_update_frames_recv++;
            client->stream_window_update_increment_recv += frame->window_update.window_size_increment;
            client->call_stream_window_update_increment_recv += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (client->first_window_update_us == 0) {
                client->first_window_update_us = elapsed;
            }
            client->last_window_update_us = elapsed;
        }
    }
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

static zend_long header_value_to_long(const uint8_t *value, size_t valuelen)
{
    char buf[32];
    size_t copy_len = valuelen < sizeof(buf) - 1 ? valuelen : sizeof(buf) - 1;
    memcpy(buf, value, copy_len);
    buf[copy_len] = '\0';
    return (zend_long) atoll(buf);
}

static void record_data_sent(poc_client *client)
{
    uint64_t elapsed = monotonic_us() - client->call_started_us;
    if (client->first_data_sent_us == 0) {
        client->first_data_sent_us = elapsed;
    }
    client->last_data_sent_us = elapsed;
}

static int receive_available(nghttp2_session *session, poc_client *client, char *recv_buf, size_t recv_buf_len)
{
    for (;;) {
        ssize_t nread = recv(client->fd, recv_buf, recv_buf_len, 0);
        if (nread > 0) {
            int rv;
            client->bytes_received += (size_t) nread;
            rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
            if (rv < 0) {
                return rv;
            }
            continue;
        }
        if (nread == 0) {
            return NGHTTP2_ERR_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            client->recv_wouldblock_calls++;
            return 0;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
}

static int drive_stream_poll(nghttp2_session *session, poc_client *client, char *recv_buf, size_t recv_buf_len)
{
    while (!client->stream_closed && (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))) {
        int rv;

        do {
            client->last_send_wouldblock = false;
            rv = nghttp2_session_send(session);
            if (rv < 0) {
                return rv;
            }
        } while (!client->last_send_wouldblock && nghttp2_session_want_write(session));

        rv = receive_available(session, client, recv_buf, recv_buf_len);
        if (rv < 0) {
            return rv;
        }
        if (client->stream_closed) {
            return 0;
        }

        short events = 0;
        if (nghttp2_session_want_read(session)) {
            events |= POLLIN;
        }
        if (nghttp2_session_want_write(session)) {
            events |= POLLOUT;
        }
        if (events == 0) {
            break;
        }

        struct pollfd pfd;
        pfd.fd = client->fd;
        pfd.events = events;
        pfd.revents = 0;
        client->poll_calls++;
        rv = poll(&pfd, 1, 5000);
        if (rv == 0) {
            client->poll_timeouts++;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->poll_errors++;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }

    return client->stream_closed ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
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
    bool no_copy = false;
    bool poll_loop = false;
    zend_long data_frame_size = 0;
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
    zval client_first_data_sent_us;
    zval client_upload_complete_us;
    zval client_first_window_update_us;
    zval client_last_window_update_us;
    zval client_first_flow_control_pause_us;
    zval client_response_header_us;
    zval client_stream_close_us;
    zval call_window_update_frames_recv;
    zval call_connection_window_update_frames_recv;
    zval call_stream_window_update_frames_recv;
    zval call_connection_window_update_increment_recv;
    zval call_stream_window_update_increment_recv;
    zval call_data_read_length_calls;
    zval call_flow_control_pauses;
    zval call_max_write_syscall_us;
    zval call_min_session_remote_window;
    zval call_min_stream_remote_window;
    zval server_handler_ns;
    zval server_payload_alloc_ns;
    zval server_payload_bytes;
    zval server_request_payload_bytes;
    zval server_stats_handler_start_ns;
    zval server_stats_handler_end_ns;
    zval server_stats_in_payload_ns;
    zval server_stats_out_header_ns;
    zval server_stats_out_payload_ns;
    zval server_stats_out_payload_bytes;
    zval server_stats_out_payload_wire_bytes;
    zval server_stats_out_payload_compressed_bytes;

    ZEND_PARSE_PARAMETERS_START(5, 10)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_LONG(iterations)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(headers_zv)
        Z_PARAM_BOOL(split_grpc_frame)
        Z_PARAM_BOOL(no_copy)
        Z_PARAM_LONG(data_frame_size)
        Z_PARAM_BOOL(poll_loop)
    ZEND_PARSE_PARAMETERS_END();

    if (iterations < 1) {
        zend_throw_exception(NULL, "iterations must be positive", 0);
        RETURN_THROWS();
    }
    if (poll_loop && no_copy) {
        zend_throw_exception(NULL, "poll loop is not supported with no-copy DATA callback", 0);
        RETURN_THROWS();
    }

    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    client.no_copy = no_copy;
    client.poll_loop = poll_loop;
    if (data_frame_size > 0) {
        client.data_frame_size_cap = (uint32_t) data_frame_size;
    }
    if (split_grpc_frame) {
        set_grpc_header(&client, request_len);
    }

    client.fd = connect_tcp(host, port);
    if (client.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }
    if (poll_loop && set_nonblocking(client.fd) != 0) {
        close(client.fd);
        zend_throw_exception(NULL, "failed to set nonblocking", 0);
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
    nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, data_source_read_length_callback);
    nghttp2_session_callbacks_set_send_data_callback(callbacks, send_data_callback);
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
    array_init(&client_first_data_sent_us);
    array_init(&client_upload_complete_us);
    array_init(&client_first_window_update_us);
    array_init(&client_last_window_update_us);
    array_init(&client_first_flow_control_pause_us);
    array_init(&client_response_header_us);
    array_init(&client_stream_close_us);
    array_init(&call_window_update_frames_recv);
    array_init(&call_connection_window_update_frames_recv);
    array_init(&call_stream_window_update_frames_recv);
    array_init(&call_connection_window_update_increment_recv);
    array_init(&call_stream_window_update_increment_recv);
    array_init(&call_data_read_length_calls);
    array_init(&call_flow_control_pauses);
    array_init(&call_max_write_syscall_us);
    array_init(&call_min_session_remote_window);
    array_init(&call_min_stream_remote_window);
    array_init(&server_handler_ns);
    array_init(&server_payload_alloc_ns);
    array_init(&server_payload_bytes);
    array_init(&server_request_payload_bytes);
    array_init(&server_stats_handler_start_ns);
    array_init(&server_stats_handler_end_ns);
    array_init(&server_stats_in_payload_ns);
    array_init(&server_stats_out_header_ns);
    array_init(&server_stats_out_payload_ns);
    array_init(&server_stats_out_payload_bytes);
    array_init(&server_stats_out_payload_wire_bytes);
    array_init(&server_stats_out_payload_compressed_bytes);
    total_started = monotonic_us();

    for (zend_long i = 0; i < iterations; i++) {
        uint64_t started = monotonic_us();
        client.call_started_us = started;
        client.stream_closed = false;
        client.grpc_status = -1;
        client.http_status = -1;
        client.stream_error_code = 0;
        client.request_offset = 0;
        client.pending_data_len = 0;
        client.first_data_sent_us = 0;
        client.last_data_sent_us = 0;
        client.first_window_update_us = 0;
        client.last_window_update_us = 0;
        client.first_flow_control_pause_us = 0;
        client.first_response_header_us = 0;
        client.stream_closed_us = 0;
        client.call_window_update_frames_recv = 0;
        client.call_connection_window_update_frames_recv = 0;
        client.call_stream_window_update_frames_recv = 0;
        client.call_connection_window_update_increment_recv = 0;
        client.call_stream_window_update_increment_recv = 0;
        client.call_data_read_length_calls = 0;
        client.call_flow_control_pauses = 0;
        client.call_max_write_syscall_us = 0;
        client.call_min_session_remote_window = 0;
        client.call_min_stream_remote_window = 0;
        client.server_handler_ns = 0;
        client.server_payload_alloc_ns = 0;
        client.server_payload_bytes = 0;
        client.server_request_payload_bytes = 0;
        client.server_stats_handler_start_ns = 0;
        client.server_stats_handler_end_ns = 0;
        client.server_stats_in_payload_ns = 0;
        client.server_stats_out_header_ns = 0;
        client.server_stats_out_payload_ns = 0;
        client.server_stats_out_payload_bytes = 0;
        client.server_stats_out_payload_wire_bytes = 0;
        client.server_stats_out_payload_compressed_bytes = 0;
        smart_str_free(&client.body);
        memset(&client.body, 0, sizeof(client.body));

        client.stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, NULL);
        if (client.stream_id < 0) {
            failed++;
            break;
        }

        if (poll_loop) {
            rv = drive_stream_poll(session, &client, recv_buf, sizeof(recv_buf));
            if (rv != 0) {
                failed++;
                break;
            }
        } else {
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
        }

        add_next_index_long(&latencies, (zend_long) (monotonic_us() - started));
        add_next_index_long(&client_first_data_sent_us, (zend_long) client.first_data_sent_us);
        add_next_index_long(&client_upload_complete_us, (zend_long) client.last_data_sent_us);
        add_next_index_long(&client_first_window_update_us, (zend_long) client.first_window_update_us);
        add_next_index_long(&client_last_window_update_us, (zend_long) client.last_window_update_us);
        add_next_index_long(&client_first_flow_control_pause_us, (zend_long) client.first_flow_control_pause_us);
        add_next_index_long(&client_response_header_us, (zend_long) client.first_response_header_us);
        add_next_index_long(&client_stream_close_us, (zend_long) client.stream_closed_us);
        add_next_index_long(&call_window_update_frames_recv, (zend_long) client.call_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_frames_recv, (zend_long) client.call_connection_window_update_frames_recv);
        add_next_index_long(&call_stream_window_update_frames_recv, (zend_long) client.call_stream_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_increment_recv, (zend_long) client.call_connection_window_update_increment_recv);
        add_next_index_long(&call_stream_window_update_increment_recv, (zend_long) client.call_stream_window_update_increment_recv);
        add_next_index_long(&call_data_read_length_calls, (zend_long) client.call_data_read_length_calls);
        add_next_index_long(&call_flow_control_pauses, (zend_long) client.call_flow_control_pauses);
        add_next_index_long(&call_max_write_syscall_us, (zend_long) client.call_max_write_syscall_us);
        add_next_index_long(&call_min_session_remote_window, (zend_long) client.call_min_session_remote_window);
        add_next_index_long(&call_min_stream_remote_window, (zend_long) client.call_min_stream_remote_window);
        add_next_index_long(&server_handler_ns, client.server_handler_ns);
        add_next_index_long(&server_payload_alloc_ns, client.server_payload_alloc_ns);
        add_next_index_long(&server_payload_bytes, client.server_payload_bytes);
        add_next_index_long(&server_request_payload_bytes, client.server_request_payload_bytes);
        add_next_index_long(&server_stats_handler_start_ns, client.server_stats_handler_start_ns);
        add_next_index_long(&server_stats_handler_end_ns, client.server_stats_handler_end_ns);
        add_next_index_long(&server_stats_in_payload_ns, client.server_stats_in_payload_ns);
        add_next_index_long(&server_stats_out_header_ns, client.server_stats_out_header_ns);
        add_next_index_long(&server_stats_out_payload_ns, client.server_stats_out_payload_ns);
        add_next_index_long(&server_stats_out_payload_bytes, client.server_stats_out_payload_bytes);
        add_next_index_long(&server_stats_out_payload_wire_bytes, client.server_stats_out_payload_wire_bytes);
        add_next_index_long(&server_stats_out_payload_compressed_bytes, client.server_stats_out_payload_compressed_bytes);
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
    add_assoc_bool(return_value, "no_copy", no_copy);
    add_assoc_bool(return_value, "poll_loop", poll_loop);
    add_assoc_long(return_value, "request_wire_bytes", client.grpc_header_len + client.request_len);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_long(return_value, "data_frames_sent", client.data_frames_sent);
    add_assoc_long(return_value, "data_bytes_sent", client.data_bytes_sent);
    add_assoc_long(return_value, "window_update_frames_recv", client.window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_frames_recv", client.connection_window_update_frames_recv);
    add_assoc_long(return_value, "stream_window_update_frames_recv", client.stream_window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_increment_recv", client.connection_window_update_increment_recv);
    add_assoc_long(return_value, "stream_window_update_increment_recv", client.stream_window_update_increment_recv);
    add_assoc_long(return_value, "flow_control_pauses", client.flow_control_pauses);
    add_assoc_long(return_value, "send_callback_calls", client.send_callback_calls);
    add_assoc_long(return_value, "send_data_callback_calls", client.send_data_callback_calls);
    add_assoc_long(return_value, "write_syscalls", client.write_syscalls);
    add_assoc_long(return_value, "send_wouldblock_calls", client.send_wouldblock_calls);
    add_assoc_long(return_value, "recv_wouldblock_calls", client.recv_wouldblock_calls);
    add_assoc_long(return_value, "poll_calls", client.poll_calls);
    add_assoc_long(return_value, "poll_timeouts", client.poll_timeouts);
    add_assoc_long(return_value, "poll_errors", client.poll_errors);
    add_assoc_long(return_value, "max_write_syscall_us", client.max_write_syscall_us);
    add_assoc_long(return_value, "data_read_calls", client.data_read_calls);
    add_assoc_long(return_value, "data_read_length_calls", client.data_read_length_calls);
    add_assoc_long(return_value, "max_send_callback_len", client.max_send_callback_len);
    add_assoc_long(return_value, "max_data_frame_len", client.max_data_frame_len);
    add_assoc_long(return_value, "min_data_frame_len", client.min_data_frame_len);
    add_assoc_long(return_value, "max_read_len", client.max_read_len);
    add_assoc_long(return_value, "min_read_len", client.min_read_len);
    add_assoc_long(return_value, "data_frame_size_cap", client.data_frame_size_cap);
    add_assoc_long(return_value, "min_session_remote_window", client.min_session_remote_window);
    add_assoc_long(return_value, "min_stream_remote_window", client.min_stream_remote_window);
    add_assoc_long(return_value, "remote_max_frame_size", client.remote_max_frame_size);
    add_assoc_zval(return_value, "latencies_us", &latencies);
    add_assoc_zval(return_value, "client_first_data_sent_us", &client_first_data_sent_us);
    add_assoc_zval(return_value, "client_upload_complete_us", &client_upload_complete_us);
    add_assoc_zval(return_value, "client_first_window_update_us", &client_first_window_update_us);
    add_assoc_zval(return_value, "client_last_window_update_us", &client_last_window_update_us);
    add_assoc_zval(return_value, "client_first_flow_control_pause_us", &client_first_flow_control_pause_us);
    add_assoc_zval(return_value, "client_response_header_us", &client_response_header_us);
    add_assoc_zval(return_value, "client_stream_close_us", &client_stream_close_us);
    add_assoc_zval(return_value, "call_window_update_frames_recv", &call_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_frames_recv", &call_connection_window_update_frames_recv);
    add_assoc_zval(return_value, "call_stream_window_update_frames_recv", &call_stream_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_increment_recv", &call_connection_window_update_increment_recv);
    add_assoc_zval(return_value, "call_stream_window_update_increment_recv", &call_stream_window_update_increment_recv);
    add_assoc_zval(return_value, "call_data_read_length_calls", &call_data_read_length_calls);
    add_assoc_zval(return_value, "call_flow_control_pauses", &call_flow_control_pauses);
    add_assoc_zval(return_value, "call_max_write_syscall_us", &call_max_write_syscall_us);
    add_assoc_zval(return_value, "call_min_session_remote_window", &call_min_session_remote_window);
    add_assoc_zval(return_value, "call_min_stream_remote_window", &call_min_stream_remote_window);
    add_assoc_zval(return_value, "server_handler_ns", &server_handler_ns);
    add_assoc_zval(return_value, "server_payload_alloc_ns", &server_payload_alloc_ns);
    add_assoc_zval(return_value, "server_payload_bytes", &server_payload_bytes);
    add_assoc_zval(return_value, "server_request_payload_bytes", &server_request_payload_bytes);
    add_assoc_zval(return_value, "server_stats_handler_start_ns", &server_stats_handler_start_ns);
    add_assoc_zval(return_value, "server_stats_handler_end_ns", &server_stats_handler_end_ns);
    add_assoc_zval(return_value, "server_stats_in_payload_ns", &server_stats_in_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_header_ns", &server_stats_out_header_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_ns", &server_stats_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_bytes", &server_stats_out_payload_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_wire_bytes", &server_stats_out_payload_wire_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_compressed_bytes", &server_stats_out_payload_compressed_bytes);
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
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, no_copy, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, data_frame_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, poll_loop, _IS_BOOL, 0, "false")
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
