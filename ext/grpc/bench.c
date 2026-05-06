#include "internal.h"

/*
 * Bench-only HTTP/2 transport entrypoints.
 *
 * This file is included from main.c intentionally. The benchmark harness uses
 * the same low-level HTTP/2 helpers and diagnostics as the production HTTP/2
 * transport, but those helpers should remain file-local instead of becoming a
 * wider extension ABI.
 */

#define MAKE_NV(NAME, VALUE) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE}
#define MAKE_NV_L(NAME, VALUE, VALUE_LEN) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, (VALUE_LEN), NGHTTP2_NV_FLAG_NONE}
#define GRPC_BENCH_MAX_RECV_BUF_SIZE 262144

static int bench_process_response_messages_from_offset(grpc_call *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us);

static void bench_observe_payload_copy(grpc_call *client, uint64_t elapsed_us)
{
    client->bench.call_response_payload_string_us += elapsed_us;
    if (elapsed_us > client->bench.call_max_response_payload_string_us) {
        client->bench.call_max_response_payload_string_us = elapsed_us;
    }
}

static void bench_observe_message_ready(grpc_call *client, uint64_t ready_abs_us)
{
    if (client->bench.call_started_us == 0 || ready_abs_us < client->bench.call_started_us) {
        return;
    }
    uint64_t ready_us = ready_abs_us - client->bench.call_started_us;
    if (client->bench.first_response_message_ready_us == 0) {
        client->bench.first_response_message_ready_us = ready_us;
    }
    client->bench.last_response_message_ready_us = ready_us;
}

static void bench_observe_payload_queued(grpc_call *client)
{
    if (client->response_queue_count > client->bench.call_max_response_queue_count) {
        client->bench.call_max_response_queue_count = client->response_queue_count;
    }
    if (client->response_queue_bytes > client->bench.call_max_response_queue_bytes) {
        client->bench.call_max_response_queue_bytes = client->response_queue_bytes;
    }
}

static void bench_observe_payload_delivered(grpc_call *client, uint64_t ready_abs_us, uint64_t callback_started_abs_us, uint64_t elapsed_us)
{
    uint64_t queue_wait = ready_abs_us > 0 && callback_started_abs_us >= ready_abs_us ? callback_started_abs_us - ready_abs_us : 0;

    client->bench.call_response_queue_wait_us += queue_wait;
    if (queue_wait > client->bench.call_max_response_queue_wait_us) {
        client->bench.call_max_response_queue_wait_us = queue_wait;
    }
    if (client->bench.call_started_us != 0) {
        uint64_t done_us = monotonic_us() - client->bench.call_started_us;
        if (client->bench.first_response_callback_done_us == 0) {
            client->bench.first_response_callback_done_us = done_us;
        }
        client->bench.last_response_callback_done_us = done_us;
    }
    client->bench.call_response_decode_us += elapsed_us;
    if (elapsed_us > client->bench.call_max_response_decode_us) {
        client->bench.call_max_response_decode_us = elapsed_us;
    }
    client->bench.call_decoded_messages++;
}

static void bench_observe_response_callback_done(grpc_call *client)
{
    if (client->bench.call_started_us == 0) {
        return;
    }
    uint64_t done_us = monotonic_us() - client->bench.call_started_us;
    if (client->bench.first_response_callback_done_us == 0) {
        client->bench.first_response_callback_done_us = done_us;
    }
    client->bench.last_response_callback_done_us = done_us;
}

static int bench_flush_queue_if_limited(grpc_call *client)
{
    bool over_message_limit = client->bench.read_ahead_max_messages > 0 && client->response_queue_count >= client->bench.read_ahead_max_messages;
    bool over_byte_limit = client->bench.read_ahead_max_bytes > 0 && client->response_queue_bytes >= client->bench.read_ahead_max_bytes;

    if (!over_message_limit && !over_byte_limit) {
        return 0;
    }

    return deliver_queued_response_payloads(client);
}

static void bench_record_data_sent(grpc_call *client)
{
    uint64_t elapsed = monotonic_us() - client->bench.call_started_us;
    if (client->bench.first_data_sent_us == 0) {
        client->bench.first_data_sent_us = elapsed;
    }
    client->bench.last_data_sent_us = elapsed;
}

static void bench_compact_response_body_if_needed(grpc_call *client)
{
    zend_string *body;
    size_t len;
    size_t consumed;
    size_t remaining;
    uint64_t started;
    uint64_t elapsed;

    if (!client->bench.compact_response_buffer || client->body.s == NULL || client->response_parse_offset == 0) {
        return;
    }
    if (client->response_parse_offset < client->bench.response_compact_threshold) {
        return;
    }

    body = client->body.s;
    len = ZSTR_LEN(body);
    consumed = client->response_parse_offset;
    if (consumed > len) {
        consumed = len;
    }
    remaining = len - consumed;

    started = monotonic_us();
    if (remaining > 0) {
        memmove(ZSTR_VAL(body), ZSTR_VAL(body) + consumed, remaining);
    }
    ZSTR_LEN(body) = remaining;
    ZSTR_VAL(body)[remaining] = '\0';
    elapsed = monotonic_us() - started;

    client->response_parse_offset = 0;
    client->bench.call_body_compact_count++;
    client->bench.call_body_compact_bytes += consumed;
    client->bench.call_body_compact_us += elapsed;
    if (elapsed > client->bench.call_max_body_compact_us) {
        client->bench.call_max_body_compact_us = elapsed;
    }
}

static int bench_process_response_messages(grpc_call *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long *decoded_messages, uint64_t *decode_us, uint64_t *max_decode_us)
{
    size_t offset = 0;
    uint64_t payload_string_us = 0;
    uint64_t max_payload_string_us = 0;
    return bench_process_response_messages_from_offset(client, fci, fcc, &offset, true, decoded_messages, &payload_string_us, &max_payload_string_us, decode_us, max_decode_us);
}

static int bench_process_response_messages_from_offset(grpc_call *client, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us)
{
    zend_string *body;
    const char *data;
    size_t len;

    *decoded_messages = 0;
    *payload_string_us = 0;
    *max_payload_string_us = 0;
    *decode_us = 0;
    *max_decode_us = 0;

    if (client->body.s == NULL) {
        return 0;
    }

    smart_str_0(&client->body);
    body = client->body.s;
    data = ZSTR_VAL(body);
    len = ZSTR_LEN(body);

    while (*offset < len) {
        uint32_t payload_len;
        zval params[1];
        zval retval;
        uint64_t payload_string_started;
        uint64_t payload_string_elapsed;
        uint64_t started;
        uint64_t elapsed;

        if (len - *offset < 5) {
            return require_complete ? -1 : 0;
        }
        payload_len = ((uint32_t) (unsigned char) data[*offset + 1] << 24)
            | ((uint32_t) (unsigned char) data[*offset + 2] << 16)
            | ((uint32_t) (unsigned char) data[*offset + 3] << 8)
            | (uint32_t) (unsigned char) data[*offset + 4];
        if (payload_len > len - *offset - 5) {
            return require_complete ? -1 : 0;
        }
        *offset += 5;

        bench_observe_message_ready(client, monotonic_us());
        payload_string_started = monotonic_us();
        ZVAL_STRINGL(&params[0], data + *offset, payload_len);
        payload_string_elapsed = monotonic_us() - payload_string_started;
        *payload_string_us += payload_string_elapsed;
        if (payload_string_elapsed > *max_payload_string_us) {
            *max_payload_string_us = payload_string_elapsed;
        }
        ZVAL_UNDEF(&retval);
        // cppcheck-suppress autoVariables
        fci->params = params;
        fci->param_count = 1;
        // cppcheck-suppress autoVariables
        fci->retval = &retval;

        started = monotonic_us();
        if (zend_call_function(fci, fcc) != SUCCESS || EG(exception)) {
            zval_ptr_dtor(&params[0]);
            if (!Z_ISUNDEF(retval)) {
                zval_ptr_dtor(&retval);
            }
            return -1;
        }
        elapsed = monotonic_us() - started;
        bench_observe_response_callback_done(client);
        *decode_us += elapsed;
        if (elapsed > *max_decode_us) {
            *max_decode_us = elapsed;
        }
        (*decoded_messages)++;

        zval_ptr_dtor(&params[0]);
        if (!Z_ISUNDEF(retval)) {
            zval_ptr_dtor(&retval);
        }
        *offset += payload_len;
    }

    return 0;
}

static ssize_t bench_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    if (client == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    client->bench.send_callback_calls++;
    if (length > client->bench.max_send_callback_len) {
        client->bench.max_send_callback_len = length;
    }

    if (client->bench.poll_loop) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = channel_send(client, data, length);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->bench.max_write_syscall_us) {
            client->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->bench.call_max_write_syscall_us) {
            client->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                client->bench.last_send_wouldblock = true;
                client->bench.send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            client->bench.last_send_wouldblock = true;
            client->bench.send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        client->bench.write_syscalls++;
        client->bytes_sent += (size_t) written;
        return written;
    }

    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = channel_send(client, data + total_written, length - total_written);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->bench.max_write_syscall_us) {
            client->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->bench.call_max_write_syscall_us) {
            client->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        client->bench.write_syscalls++;
        total_written += (size_t) written;
    }
    client->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static int bench_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    (void) flags;
    if (stream_id == client->stream_id && len > 0) {
        uint64_t elapsed = client->bench.call_started_us == 0 ? 0 : monotonic_us() - client->bench.call_started_us;
        client->data_recv_calls++;
        client->bench.call_data_recv_calls++;
        client->bench.response_data_bytes += len;
        client->bench.call_response_data_bytes += len;
        if (elapsed > 0) {
            if (client->bench.first_response_data_us == 0) {
                client->bench.first_response_data_us = elapsed;
            }
            client->bench.last_response_data_us = elapsed;
        }
        if (client->bench.awaiting_data_after_poll && client->bench.last_poll_return_abs_us > 0) {
            uint64_t delta = monotonic_us() - client->bench.last_poll_return_abs_us;
            client->bench.call_poll_to_data_us += delta;
            if (delta > client->bench.call_max_poll_to_data_us) {
                client->bench.call_max_poll_to_data_us = delta;
            }
            client->bench.awaiting_data_after_poll = false;
        }
        if (client->bench.awaiting_data_after_window_update_sent && client->bench.last_window_update_sent_abs_us > 0) {
            uint64_t delta = monotonic_us() - client->bench.last_window_update_sent_abs_us;
            client->bench.call_window_update_to_data_us += delta;
            if (delta > client->bench.call_max_window_update_to_data_us) {
                client->bench.call_max_window_update_to_data_us = delta;
            }
            client->bench.awaiting_data_after_window_update_sent = false;
        }
        if (client->direct_response_payload && client->decode_response_incrementally && ((client->payload_callback_fci != NULL && client->payload_callback_fcc != NULL) || client->queue_response_payloads)) {
            if (process_response_data_direct(session, client, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        } else if (!client->discard_response_body) {
            if (validate_response_message_lengths(session, client, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (client->discard_response_body) {
                return 0;
            }
            uint64_t append_started = monotonic_us();
            uint64_t append_elapsed;
            smart_str_appendl(&client->body, (const char *) data, len);
            append_elapsed = monotonic_us() - append_started;
            if (client->body.s != NULL && ZSTR_LEN(client->body.s) > client->bench.call_max_body_buffer_bytes) {
                client->bench.call_max_body_buffer_bytes = ZSTR_LEN(client->body.s);
            }
            client->bench.call_body_append_us += append_elapsed;
            if (append_elapsed > client->bench.call_max_body_append_us) {
                client->bench.call_max_body_append_us = append_elapsed;
            }
            if (client->decode_response_incrementally && client->payload_callback_fci != NULL && client->payload_callback_fcc != NULL) {
                zend_long decoded_messages = 0;
                uint64_t payload_string_us = 0;
                uint64_t max_payload_string_us = 0;
                uint64_t decode_us = 0;
                uint64_t max_decode_us = 0;
                if (bench_process_response_messages_from_offset(client, client->payload_callback_fci, client->payload_callback_fcc, &client->response_parse_offset, false, &decoded_messages, &payload_string_us, &max_payload_string_us, &decode_us, &max_decode_us) != 0) {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
                client->bench.call_decoded_messages += decoded_messages;
                client->bench.call_response_payload_string_us += payload_string_us;
                if (max_payload_string_us > client->bench.call_max_response_payload_string_us) {
                    client->bench.call_max_response_payload_string_us = max_payload_string_us;
                }
                client->bench.call_response_decode_us += decode_us;
                if (max_decode_us > client->bench.call_max_response_decode_us) {
                    client->bench.call_max_response_decode_us = max_decode_us;
                }
                bench_compact_response_body_if_needed(client);
            }
        }
    }
    return 0;
}

static int bench_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    bool trailing;
    (void) session;
    (void) flags;
    if (frame->hd.stream_id != client->stream_id) {
        return 0;
    }
    trailing = frame->headers.cat != NGHTTP2_HCAT_RESPONSE;
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        client->grpc_status = parse_grpc_status_value(value, valuelen);
        if (client->grpc_status < 0) {
            client->invalid_grpc_status = true;
        }
        trailing = true;
    } else if (namelen == sizeof("grpc-message") - 1 && memcmp(name, "grpc-message", namelen) == 0) {
        if (client->grpc_message != NULL) {
            zend_string_release(client->grpc_message);
        }
        client->grpc_message = zend_string_init((const char *) value, valuelen, 0);
        trailing = true;
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        if (client->bench.first_response_header_us == 0) {
            client->bench.first_response_header_us = monotonic_us() - client->bench.call_started_us;
        }
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        client->http_status = atoi(status_buf);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    } else if (namelen == sizeof("x-bench-server-handler-ns") - 1 && memcmp(name, "x-bench-server-handler-ns", namelen) == 0) {
        client->bench.server_handler_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-alloc-ns") - 1 && memcmp(name, "x-bench-server-payload-alloc-ns", namelen) == 0) {
        client->bench.server_payload_alloc_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-bytes") - 1 && memcmp(name, "x-bench-server-payload-bytes", namelen) == 0) {
        client->bench.server_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-request-payload-bytes") - 1 && memcmp(name, "x-bench-server-request-payload-bytes", namelen) == 0) {
        client->bench.server_request_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-start-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-start-ns", namelen) == 0) {
        client->bench.server_stats_handler_start_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-end-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-end-ns", namelen) == 0) {
        client->bench.server_stats_handler_end_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-in-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-in-payload-ns", namelen) == 0) {
        client->bench.server_stats_in_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-header-ns") - 1 && memcmp(name, "x-bench-server-stats-out-header-ns", namelen) == 0) {
        client->bench.server_stats_out_header_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-out-payload-ns", namelen) == 0) {
        client->bench.server_stats_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-first-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-first-out-payload-ns", namelen) == 0) {
        client->bench.server_stats_first_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-last-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-last-out-payload-ns", namelen) == 0) {
        client->bench.server_stats_last_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-count") - 1 && memcmp(name, "x-bench-server-stats-out-payload-count", namelen) == 0) {
        client->bench.server_stats_out_payload_count = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-bytes", namelen) == 0) {
        client->bench.server_stats_out_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-wire-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-wire-bytes", namelen) == 0) {
        client->bench.server_stats_out_payload_wire_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-compressed-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-compressed-bytes", namelen) == 0) {
        client->bench.server_stats_out_payload_compressed_bytes = header_value_to_long(value, valuelen);
#endif
    }
    if (add_metadata_entry(client, name, namelen, value, valuelen, trailing) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int bench_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    (void) error_code;
    if (stream_id == client->stream_id) {
        client->stream_closed = true;
        client->stream_error_code = error_code;
        client->bench.stream_closed_us = monotonic_us() - client->bench.call_started_us;
    }
    return 0;
}

static int bench_on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->sent_frames++;
    client->last_sent_frame_type = frame->hd.type;
    client->last_sent_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_DATA && !client->bench.no_copy) {
        client->bench.data_frames_sent++;
        client->bench.data_bytes_sent += frame->hd.length;
        bench_record_data_sent(client);
        if (frame->hd.length > client->bench.max_data_frame_len) {
            client->bench.max_data_frame_len = frame->hd.length;
        }
        if (client->bench.min_data_frame_len == 0 || frame->hd.length < client->bench.min_data_frame_len) {
            client->bench.min_data_frame_len = frame->hd.length;
        }
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = client->bench.call_started_us == 0 ? 0 : monotonic_us() - client->bench.call_started_us;
        client->bench.window_update_frames_sent++;
        client->bench.call_window_update_frames_sent++;
        if (frame->hd.stream_id == 0) {
            client->bench.connection_window_update_frames_sent++;
            client->bench.call_connection_window_update_frames_sent++;
            client->bench.connection_window_update_increment_sent += frame->window_update.window_size_increment;
            client->bench.call_connection_window_update_increment_sent += frame->window_update.window_size_increment;
        } else {
            client->bench.stream_window_update_frames_sent++;
            client->bench.call_stream_window_update_frames_sent++;
            client->bench.stream_window_update_increment_sent += frame->window_update.window_size_increment;
            client->bench.call_stream_window_update_increment_sent += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (client->bench.first_window_update_sent_us == 0) {
                client->bench.first_window_update_sent_us = elapsed;
            }
            client->bench.last_window_update_sent_us = elapsed;
            client->bench.last_window_update_sent_abs_us = monotonic_us();
            client->bench.awaiting_data_after_window_update_sent = true;
        }
    }
    return 0;
}

static int bench_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->recv_frames++;
    client->last_recv_frame_type = frame->hd.type;
    client->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        mark_channel_draining(client->channel, frame->goaway.last_stream_id, frame->goaway.error_code);
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = client->bench.call_started_us == 0 ? 0 : monotonic_us() - client->bench.call_started_us;
        client->bench.window_update_frames_recv++;
        client->bench.call_window_update_frames_recv++;
        if (frame->hd.stream_id == 0) {
            client->bench.connection_window_update_frames_recv++;
            client->bench.call_connection_window_update_frames_recv++;
            client->bench.connection_window_update_increment_recv += frame->window_update.window_size_increment;
            client->bench.call_connection_window_update_increment_recv += frame->window_update.window_size_increment;
        } else {
            client->bench.stream_window_update_frames_recv++;
            client->bench.call_stream_window_update_frames_recv++;
            client->bench.stream_window_update_increment_recv += frame->window_update.window_size_increment;
            client->bench.call_stream_window_update_increment_recv += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (client->bench.first_window_update_us == 0) {
                client->bench.first_window_update_us = elapsed;
            }
            client->bench.last_window_update_us = elapsed;
        }
    }
    return 0;
}

static int bench_on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    (void) session;
    client->not_sent_frames++;
    client->last_not_sent_frame_type = frame->hd.type;
    client->last_not_sent_error = lib_error_code;
    return 0;
}


static ssize_t data_source_read_length_callback(nghttp2_session *session, uint8_t frame_type, int32_t stream_id, int32_t session_remote_window_size, int32_t stream_remote_window_size, uint32_t remote_max_frame_size, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
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
    if (client->bench.data_frame_size_cap > 0 && client->bench.data_frame_size_cap < allowed) {
        allowed = client->bench.data_frame_size_cap;
    }
    if (allowed == 0) {
        uint64_t elapsed = client->bench.call_started_us == 0 ? 0 : monotonic_us() - client->bench.call_started_us;
        client->bench.flow_control_pauses++;
        client->bench.call_flow_control_pauses++;
        if (elapsed > 0 && client->bench.first_flow_control_pause_us == 0) {
            client->bench.first_flow_control_pause_us = elapsed;
        }
        return NGHTTP2_ERR_PAUSE;
    }

    client->bench.data_read_length_calls++;
    client->bench.call_data_read_length_calls++;
    client->bench.remote_max_frame_size = remote_max_frame_size;
    if (client->bench.min_session_remote_window == 0 || session_remote_window_size < client->bench.min_session_remote_window) {
        client->bench.min_session_remote_window = session_remote_window_size;
    }
    if (client->bench.min_stream_remote_window == 0 || stream_remote_window_size < client->bench.min_stream_remote_window) {
        client->bench.min_stream_remote_window = stream_remote_window_size;
    }
    if (client->bench.call_min_session_remote_window == 0 || session_remote_window_size < client->bench.call_min_session_remote_window) {
        client->bench.call_min_session_remote_window = session_remote_window_size;
    }
    if (client->bench.call_min_stream_remote_window == 0 || stream_remote_window_size < client->bench.call_min_stream_remote_window) {
        client->bench.call_min_stream_remote_window = stream_remote_window_size;
    }
    if (allowed > client->bench.max_read_len) {
        client->bench.max_read_len = allowed;
    }
    if (client->bench.min_read_len == 0 || allowed < client->bench.min_read_len) {
        client->bench.min_read_len = allowed;
    }

    return (ssize_t) allowed;
}

static size_t fill_request_iov(grpc_call *client, struct iovec *iov, size_t iov_offset, size_t length, size_t *filled_len)
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

static int write_data_frame(grpc_call *client, const uint8_t *framehd, size_t length)
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
        if (syscall_elapsed > client->bench.max_write_syscall_us) {
            client->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->bench.call_max_write_syscall_us) {
            client->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        client->bench.write_syscalls++;
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

static void clear_pending_write(grpc_call *client)
{
    client->pending_write_iovcnt = 0;
    client->pending_write_remaining = 0;
    client->pending_write_payload_len = 0;
}

static void consume_pending_write(grpc_call *client, size_t consumed)
{
    while (client->pending_write_iovcnt > 0 && consumed > 0) {
        struct iovec *iov = &client->pending_write_iov[0];
        if (consumed >= iov->iov_len) {
            consumed -= iov->iov_len;
            client->pending_write_remaining -= iov->iov_len;
            memmove(&client->pending_write_iov[0], &client->pending_write_iov[1], sizeof(struct iovec) * (client->pending_write_iovcnt - 1));
            client->pending_write_iovcnt--;
            continue;
        }

        iov->iov_base = (uint8_t *) iov->iov_base + consumed;
        iov->iov_len -= consumed;
        client->pending_write_remaining -= consumed;
        consumed = 0;
    }
}

static int prepare_pending_data_frame_write(grpc_call *client, const uint8_t *framehd, size_t length)
{
    size_t iovcnt = 1;
    size_t filled_len = 0;

    client->pending_write_iov[0].iov_base = (void *) framehd;
    client->pending_write_iov[0].iov_len = 9;
    iovcnt = fill_request_iov(client, client->pending_write_iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        clear_pending_write(client);
        return -1;
    }

    client->pending_write_iovcnt = iovcnt;
    client->pending_write_remaining = 9 + length;
    client->pending_write_payload_len = length;
    return 0;
}

static int flush_pending_data_frame_write(grpc_call *client)
{
    while (client->pending_write_remaining > 0) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(client->fd, client->pending_write_iov, (int) client->pending_write_iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > client->bench.max_write_syscall_us) {
            client->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > client->bench.call_max_write_syscall_us) {
            client->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                client->bench.last_send_wouldblock = true;
                client->bench.send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            clear_pending_write(client);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            client->bench.last_send_wouldblock = true;
            client->bench.send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }

        client->bench.write_syscalls++;
        client->bytes_sent += (size_t) written;
        consume_pending_write(client, (size_t) written);
    }

    client->request_offset += client->pending_write_payload_len;
    clear_pending_write(client);
    return 0;
}

static int write_data_frame_nonblocking(grpc_call *client, const uint8_t *framehd, size_t length)
{
    if (client->pending_write_remaining == 0 && prepare_pending_data_frame_write(client, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return flush_pending_data_frame_write(client);
}

static int send_data_callback(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    bool new_data_frame = client->pending_write_remaining == 0;
    (void) session;
    (void) frame;
    (void) source;

    client->bench.send_data_callback_calls++;
    if (new_data_frame) {
        client->bench.data_frames_sent++;
        client->bench.data_bytes_sent += length;
        if (length > client->bench.max_data_frame_len) {
            client->bench.max_data_frame_len = length;
        }
        if (client->bench.min_data_frame_len == 0 || length < client->bench.min_data_frame_len) {
            client->bench.min_data_frame_len = length;
        }
    }

    if (client->bench.poll_loop) {
        if (write_data_frame(client, framehd, length) != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    } else if (write_data_frame(client, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    bench_record_data_sent(client);
    client->pending_data_len = 0;

    return 0;
}

static ssize_t bench_data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    grpc_call *client = (grpc_call *) user_data;
    size_t total_len = client->grpc_header_len + client->request_len;
    size_t remaining = remaining_request_bytes(client);
    size_t to_send = remaining < length ? remaining : length;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    client->data_read_calls++;

    if (client->bench.no_copy && to_send > 0) {
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

static int receive_available(nghttp2_session *session, grpc_call *client, char *recv_buf, size_t recv_buf_len)
{
    size_t reads = 0;
    size_t bytes = 0;
    client->bench.call_receive_drains++;
    for (;;) {
        uint64_t recv_started = monotonic_us();
        ssize_t nread = recv(client->fd, recv_buf, recv_buf_len, 0);
        uint64_t recv_elapsed = monotonic_us() - recv_started;
        if (nread > 0) {
            int rv;
            uint64_t mem_recv_started;
            uint64_t mem_recv_elapsed;
            client->bench.call_recv_syscalls++;
            client->bench.call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > client->bench.call_max_recv_syscall_us) {
                client->bench.call_max_recv_syscall_us = recv_elapsed;
            }
            reads++;
            bytes += (size_t) nread;
            client->bytes_received += (size_t) nread;
            mem_recv_started = monotonic_us();
            rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
            mem_recv_elapsed = monotonic_us() - mem_recv_started;
            client->bench.call_mem_recv_us += mem_recv_elapsed;
            if (mem_recv_elapsed > client->bench.call_max_mem_recv_us) {
                client->bench.call_max_mem_recv_us = mem_recv_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
            if (client->bench.flush_after_mem_recv && nghttp2_session_want_write(session)) {
                uint64_t send_started = monotonic_us();
                uint64_t send_elapsed;
                rv = nghttp2_session_send(session);
                send_elapsed = monotonic_us() - send_started;
                client->bench.call_session_send_after_recv_us += send_elapsed;
                if (send_elapsed > client->bench.call_max_session_send_after_recv_us) {
                    client->bench.call_max_session_send_after_recv_us = send_elapsed;
                }
                if (rv < 0) {
                    return rv;
                }
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
            client->bench.call_recv_syscalls++;
            client->bench.call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > client->bench.call_max_recv_syscall_us) {
                client->bench.call_max_recv_syscall_us = recv_elapsed;
            }
            client->bench.recv_wouldblock_calls++;
            if (reads > 0) {
                client->bench.call_receive_drains_with_data++;
                client->bench.call_receive_drains_eagain_after_data++;
                if (reads > client->bench.call_max_reads_per_drain) {
                    client->bench.call_max_reads_per_drain = reads;
                }
                if (bytes > client->bench.call_max_bytes_per_drain) {
                    client->bench.call_max_bytes_per_drain = bytes;
                }
            }
            return 0;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
}

static int drive_stream_poll(nghttp2_session *session, grpc_call *client, char *recv_buf, size_t recv_buf_len)
{
    while (!client->stream_closed && (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))) {
        int rv;

        if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
            client->timed_out = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        if (client->bench.read_first_poll_loop && nghttp2_session_want_read(session)) {
            rv = receive_available(session, client, recv_buf, recv_buf_len);
            if (rv < 0) {
                return rv;
            }
            if (client->bench.read_ahead_delivery && deliver_queued_response_payloads(client) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (client->stream_closed) {
                return 0;
            }
        }

        do {
            uint64_t send_started;
            uint64_t send_elapsed;
            client->bench.last_send_wouldblock = false;
            send_started = monotonic_us();
            rv = nghttp2_session_send(session);
            send_elapsed = monotonic_us() - send_started;
            client->bench.call_session_send_after_recv_us += send_elapsed;
            if (send_elapsed > client->bench.call_max_session_send_after_recv_us) {
                client->bench.call_max_session_send_after_recv_us = send_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
        } while (!client->bench.last_send_wouldblock && nghttp2_session_want_write(session));

        rv = receive_available(session, client, recv_buf, recv_buf_len);
        if (rv < 0) {
            return rv;
        }
        if (client->bench.read_ahead_delivery && deliver_queued_response_payloads(client) != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
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
        client->bench.poll_calls++;
        uint64_t poll_started = monotonic_us();
        int poll_timeout_ms = 5000;
        if (client->deadline_abs_us > 0) {
            uint64_t remaining_us;
            if (poll_started >= client->deadline_abs_us) {
                client->timed_out = true;
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            remaining_us = client->deadline_abs_us - poll_started;
            poll_timeout_ms = (int) ((remaining_us + 999) / 1000);
            if (poll_timeout_ms < 1) {
                poll_timeout_ms = 1;
            } else if (poll_timeout_ms > 5000) {
                poll_timeout_ms = 5000;
            }
        }
        rv = poll(&pfd, 1, poll_timeout_ms);
        uint64_t poll_elapsed = monotonic_us() - poll_started;
        client->bench.call_poll_wait_us += poll_elapsed;
        if (poll_elapsed > client->bench.call_max_poll_wait_us) {
            client->bench.call_max_poll_wait_us = poll_elapsed;
        }
        if (rv == 0) {
            client->bench.poll_timeouts++;
            if (client->deadline_abs_us > 0 && monotonic_us() >= client->deadline_abs_us) {
                client->timed_out = true;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->bench.poll_errors++;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if ((pfd.revents & POLLIN) != 0) {
            client->bench.call_pollin_ready++;
            client->bench.last_poll_return_abs_us = monotonic_us();
            client->bench.awaiting_data_after_poll = true;
        }
        if ((pfd.revents & POLLOUT) != 0) {
            client->bench.call_pollout_ready++;
        }
    }

    return client->stream_closed ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
}

typedef struct {
    int32_t stream_id;
    bool stream_closed;
    int grpc_status;
    uint32_t stream_error_code;
    const uint8_t *request;
    size_t request_len;
    size_t request_offset;
    smart_str body;
} mux_stream;

typedef struct {
    int fd;
    mux_stream *streams;
    size_t stream_count;
    size_t closed_count;
    size_t bytes_sent;
    size_t bytes_received;
} mux_context;

static ssize_t mux_send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    mux_context *ctx = (mux_context *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;
    while (total_written < length) {
        ssize_t written = send(ctx->fd, data + total_written, length - total_written, 0);
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        total_written += (size_t) written;
    }
    ctx->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static ssize_t mux_data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    mux_stream *stream = (mux_stream *) source->ptr;
    size_t remaining;
    size_t to_copy;
    (void) session;
    (void) stream_id;
    (void) user_data;

    *data_flags = 0;
    if (stream->request_offset >= stream->request_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    remaining = stream->request_len - stream->request_offset;
    to_copy = remaining < length ? remaining : length;
    memcpy(buf, stream->request + stream->request_offset, to_copy);
    stream->request_offset += to_copy;
    if (stream->request_offset >= stream->request_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) to_copy;
}

static int mux_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    mux_stream *stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    (void) flags;
    (void) user_data;
    if (stream != NULL && len > 0) {
        smart_str_appendl(&stream->body, (const char *) data, len);
    }
    return 0;
}

static int mux_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    mux_stream *stream;
    (void) flags;
    (void) user_data;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_HEADERS) {
        return 0;
    }
    stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (stream == NULL) {
        return 0;
    }
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        stream->grpc_status = (int) header_value_to_long(value, valuelen);
    }
    return 0;
}

static int mux_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    mux_context *ctx = (mux_context *) user_data;
    mux_stream *stream = (mux_stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    (void) session;
    if (stream != NULL && !stream->stream_closed) {
        stream->stream_closed = true;
        stream->stream_error_code = error_code;
        ctx->closed_count++;
    }
    return 0;
}

PHP_FUNCTION(grpc_lite_multiplex_unary)
{
    char *host = NULL;
    size_t host_len = 0;
    zend_long port = 0;
    char *path = NULL;
    size_t path_len = 0;
    char *request = NULL;
    size_t request_len = 0;
    zend_long stream_count = 0;
    mux_context ctx;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_nv nva[7];
    size_t nvlen = 0;
    char authority[512];
    char recv_buf[16384];
    uint64_t started;
    int rv = 0;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_LONG(port)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_STRING(request, request_len)
        Z_PARAM_LONG(stream_count)
    ZEND_PARSE_PARAMETERS_END();

    if (stream_count <= 0 || stream_count > 256) {
        zend_throw_exception(NULL, "stream_count must be between 1 and 256", 0);
        RETURN_THROWS();
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = connect_tcp(host, port, 0);
    if (ctx.fd < 0) {
        zend_throw_exception(NULL, "failed to connect", 0);
        RETURN_THROWS();
    }
    ctx.stream_count = (size_t) stream_count;
    ctx.streams = ecalloc(ctx.stream_count, sizeof(mux_stream));

    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        close(ctx.fd);
        efree(ctx.streams);
        zend_throw_exception(NULL, "failed to configure callbacks", 0);
        RETURN_THROWS();
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, mux_send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, mux_on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, mux_on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, mux_on_stream_close_callback);
    if (nghttp2_session_client_new(&session, callbacks, &ctx) != 0) {
        close(ctx.fd);
        nghttp2_session_callbacks_del(callbacks);
        efree(ctx.streams);
        zend_throw_exception(NULL, "failed to create nghttp2 session", 0);
        RETURN_THROWS();
    }
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", "php-grpc-lite/0.1.0-dev");

    for (size_t i = 0; i < ctx.stream_count; i++) {
        nghttp2_data_provider data_provider;
        memset(&data_provider, 0, sizeof(data_provider));
        ctx.streams[i].grpc_status = -1;
        ctx.streams[i].request = (const uint8_t *) request;
        ctx.streams[i].request_len = request_len;
        data_provider.source.ptr = &ctx.streams[i];
        data_provider.read_callback = mux_data_source_read_callback;
        ctx.streams[i].stream_id = nghttp2_submit_request(session, NULL, nva, nvlen, &data_provider, &ctx.streams[i]);
        if (ctx.streams[i].stream_id < 0) {
            rv = -1;
            break;
        }
    }

    started = monotonic_us();
    if (rv == 0 && nghttp2_session_send(session) != 0) {
        rv = -1;
    }
    while (rv == 0 && ctx.closed_count < ctx.stream_count) {
        ssize_t nread = recv(ctx.fd, recv_buf, sizeof(recv_buf), 0);
        if (nread <= 0) {
            rv = -1;
            break;
        }
        ctx.bytes_received += (size_t) nread;
        if (nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread) < 0) {
            rv = -1;
            break;
        }
        if (nghttp2_session_send(session) != 0) {
            rv = -1;
            break;
        }
    }

    array_init(return_value);
    add_assoc_long(return_value, "streams", (zend_long) ctx.stream_count);
    add_assoc_long(return_value, "closed", (zend_long) ctx.closed_count);
    add_assoc_long(return_value, "elapsed_us", (zend_long) (monotonic_us() - started));
    add_assoc_long(return_value, "bytes_sent", (zend_long) ctx.bytes_sent);
    add_assoc_long(return_value, "bytes_received", (zend_long) ctx.bytes_received);
    add_assoc_bool(return_value, "ok", rv == 0 && ctx.closed_count == ctx.stream_count);
    zval statuses;
    array_init(&statuses);
    for (size_t i = 0; i < ctx.stream_count; i++) {
        add_next_index_long(&statuses, ctx.streams[i].grpc_status);
        smart_str_free(&ctx.streams[i].body);
    }
    add_assoc_zval(return_value, "grpc_statuses", &statuses);

    close(ctx.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    efree(ctx.streams);
}

PHP_FUNCTION(grpc_lite_bench_unary_batch)
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
    bool discard_response_body = false;
    zend_long data_frame_size = 0;
    zend_long recv_stream_window_size = 0;
    zend_long recv_connection_window_size = 0;
    zend_long recv_buffer_size = 16384;
    bool flush_after_mem_recv = false;
    bool read_first_poll_loop = false;
    bool decode_response_incrementally = false;
    bool direct_response_payload = false;
    bool read_ahead_delivery = false;
    zend_long read_ahead_max_messages = 0;
    zend_long read_ahead_max_bytes = 0;
    zend_long timeout_us = 0;
    bool compact_response_buffer = false;
    zend_long response_compact_threshold = 0;
    zval *response_callback_zv = NULL;
    bool response_callback_enabled = false;
    zend_fcall_info response_fci;
    zend_fcall_info_cache response_fcc;
    grpc_call client;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_data_provider data_provider;
    h2_request_headers request_headers;
    char authority[512];
    int rv;
    char recv_buf[GRPC_BENCH_MAX_RECV_BUF_SIZE];
    size_t recv_buf_len = 16384;
    zend_long ok = 0;
    zend_long failed = 0;
    uint64_t total_started;
    uint64_t total_elapsed;
    zval latencies;
    zval client_first_data_sent_us;
    zval client_upload_complete_us;
    zval client_first_response_data_us;
    zval client_last_response_data_us;
    zval client_first_window_update_us;
    zval client_last_window_update_us;
    zval client_first_window_update_sent_us;
    zval client_last_window_update_sent_us;
    zval client_first_flow_control_pause_us;
    zval client_response_header_us;
    zval client_stream_close_us;
    zval client_first_response_message_ready_us;
    zval client_last_response_message_ready_us;
    zval client_first_response_callback_done_us;
    zval client_last_response_callback_done_us;
    zval call_window_update_frames_recv;
    zval call_connection_window_update_frames_recv;
    zval call_stream_window_update_frames_recv;
    zval call_connection_window_update_increment_recv;
    zval call_stream_window_update_increment_recv;
    zval call_window_update_frames_sent;
    zval call_connection_window_update_frames_sent;
    zval call_stream_window_update_frames_sent;
    zval call_connection_window_update_increment_sent;
    zval call_stream_window_update_increment_sent;
    zval call_data_read_length_calls;
    zval call_flow_control_pauses;
    zval call_max_write_syscall_us;
    zval call_recv_syscalls;
    zval call_recv_syscall_us;
    zval call_max_recv_syscall_us;
    zval call_mem_recv_us;
    zval call_max_mem_recv_us;
    zval call_session_send_after_recv_us;
    zval call_max_session_send_after_recv_us;
    zval call_poll_wait_us;
    zval call_max_poll_wait_us;
    zval call_pollin_ready;
    zval call_pollout_ready;
    zval call_poll_to_data_us;
    zval call_max_poll_to_data_us;
    zval call_window_update_to_data_us;
    zval call_max_window_update_to_data_us;
    zval call_receive_drains;
    zval call_receive_drains_with_data;
    zval call_receive_drains_eagain_after_data;
    zval call_max_reads_per_drain;
    zval call_max_bytes_per_drain;
    zval call_min_session_remote_window;
    zval call_min_stream_remote_window;
    zval call_response_data_bytes;
    zval call_data_recv_calls;
    zval call_body_append_us;
    zval call_max_body_append_us;
    zval call_body_compact_count;
    zval call_body_compact_bytes;
    zval call_body_compact_us;
    zval call_max_body_compact_us;
    zval call_max_body_buffer_bytes;
    zval call_decoded_messages;
    zval call_max_response_queue_count;
    zval call_max_response_queue_bytes;
    zval call_response_queue_wait_us;
    zval call_max_response_queue_wait_us;
    zval call_response_payload_string_us;
    zval call_max_response_payload_string_us;
    zval call_response_decode_us;
    zval call_max_response_decode_us;
    zval server_handler_ns;
    zval server_payload_alloc_ns;
    zval server_payload_bytes;
    zval server_request_payload_bytes;
    zval server_stats_handler_start_ns;
    zval server_stats_handler_end_ns;
    zval server_stats_in_payload_ns;
    zval server_stats_out_header_ns;
    zval server_stats_out_payload_ns;
    zval server_stats_first_out_payload_ns;
    zval server_stats_last_out_payload_ns;
    zval server_stats_out_payload_count;
    zval server_stats_out_payload_bytes;
    zval server_stats_out_payload_wire_bytes;
    zval server_stats_out_payload_compressed_bytes;

    memset(&response_fci, 0, sizeof(response_fci));
    memset(&response_fcc, 0, sizeof(response_fcc));

    ZEND_PARSE_PARAMETERS_START(5, 25)
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
        Z_PARAM_BOOL(discard_response_body)
        Z_PARAM_LONG(recv_stream_window_size)
        Z_PARAM_LONG(recv_connection_window_size)
        Z_PARAM_LONG(recv_buffer_size)
        Z_PARAM_BOOL(flush_after_mem_recv)
        Z_PARAM_BOOL(read_first_poll_loop)
        Z_PARAM_ZVAL_OR_NULL(response_callback_zv)
        Z_PARAM_BOOL(decode_response_incrementally)
        Z_PARAM_BOOL(compact_response_buffer)
        Z_PARAM_LONG(response_compact_threshold)
        Z_PARAM_BOOL(direct_response_payload)
        Z_PARAM_BOOL(read_ahead_delivery)
        Z_PARAM_LONG(read_ahead_max_messages)
        Z_PARAM_LONG(read_ahead_max_bytes)
        Z_PARAM_LONG(timeout_us)
    ZEND_PARSE_PARAMETERS_END();

    if (iterations < 1) {
        zend_throw_exception(NULL, "iterations must be positive", 0);
        RETURN_THROWS();
    }
    if (timeout_us < 0) {
        zend_throw_exception(NULL, "timeout must be non-negative microseconds", 0);
        RETURN_THROWS();
    }
    if (data_frame_size < 0 || data_frame_size > 0x3fff) {
        zend_throw_exception(NULL, "data_frame_size must be between 0 and 16383", 0);
        RETURN_THROWS();
    }
    if (recv_stream_window_size < 0 || recv_connection_window_size < 0 || recv_buffer_size < 0) {
        zend_throw_exception(NULL, "receive sizes must be non-negative", 0);
        RETURN_THROWS();
    }
    if (response_callback_zv != NULL && Z_TYPE_P(response_callback_zv) != IS_NULL) {
        if (zend_fcall_info_init(response_callback_zv, 0, &response_fci, &response_fcc, NULL, NULL) != SUCCESS) {
            zend_throw_exception(NULL, "response callback must be callable", 0);
            RETURN_THROWS();
        }
        response_callback_enabled = true;
        discard_response_body = false;
    }
    memset(&client, 0, sizeof(client));
    client.fd = -1;
    client.grpc_status = -1;
    client.http_status = -1;
    client.request = (const uint8_t *) request;
    client.request_len = request_len;
    client.bench.no_copy = no_copy;
    client.bench.poll_loop = poll_loop;
    client.discard_response_body = discard_response_body;
    client.bench.discard_response_body = discard_response_body;
    client.bench.flush_after_mem_recv = flush_after_mem_recv;
    client.bench.read_first_poll_loop = read_first_poll_loop;
    client.decode_response_incrementally = decode_response_incrementally;
    client.direct_response_payload = direct_response_payload && decode_response_incrementally && response_callback_enabled;
    client.bench.read_ahead_delivery = read_ahead_delivery && client.direct_response_payload;
    client.queue_response_payloads = client.bench.read_ahead_delivery;
    client.bench.read_ahead_max_messages = read_ahead_max_messages > 0 ? (size_t) read_ahead_max_messages : 0;
    client.bench.read_ahead_max_bytes = read_ahead_max_bytes > 0 ? (size_t) read_ahead_max_bytes : 0;
    client.bench.compact_response_buffer = compact_response_buffer && decode_response_incrementally && !client.direct_response_payload;
    client.bench.response_compact_threshold = response_compact_threshold > 0 ? (size_t) response_compact_threshold : 1;
    client.observe_payload_copy = bench_observe_payload_copy;
    client.observe_message_ready = bench_observe_message_ready;
    client.observe_payload_queued = bench_observe_payload_queued;
    client.observe_payload_delivered = bench_observe_payload_delivered;
    client.flush_queue_if_limited = bench_flush_queue_if_limited;
    if (response_callback_enabled) {
        client.payload_callback_fci = &response_fci;
        client.payload_callback_fcc = &response_fcc;
    }
    if (data_frame_size > 0) {
        client.bench.data_frame_size_cap = (uint32_t) data_frame_size;
    }
    if (recv_stream_window_size > 0) {
        client.bench.recv_stream_window_size = (uint32_t) recv_stream_window_size;
    }
    if (recv_connection_window_size > 0) {
        client.bench.recv_connection_window_size = (uint32_t) recv_connection_window_size;
    }
    if (recv_buffer_size > 0) {
        recv_buf_len = (size_t) recv_buffer_size;
        if (recv_buf_len > GRPC_BENCH_MAX_RECV_BUF_SIZE) {
            recv_buf_len = GRPC_BENCH_MAX_RECV_BUF_SIZE;
        }
    }
    if (split_grpc_frame) {
        set_grpc_header(&client, request_len);
    }

    client.deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    client.fd = connect_tcp(host, port, client.deadline_abs_us);
    if (client.fd < 0) {
        zend_throw_exception(NULL, errno == ETIMEDOUT ? "HTTP/2 transport deadline exceeded" : "failed to connect", 0);
        RETURN_THROWS();
    }
    if (poll_loop && set_nonblocking(client.fd) != 0) {
        close(client.fd);
        zend_throw_exception(NULL, "failed to set nonblocking", 0);
        RETURN_THROWS();
    }

    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        close(client.fd);
        zend_throw_exception(NULL, "failed to configure callbacks", 0);
        RETURN_THROWS();
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, bench_send_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, bench_on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, bench_on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, bench_on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, bench_on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, bench_on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, bench_on_frame_not_send_callback);
    nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, data_source_read_length_callback);
    nghttp2_session_callbacks_set_send_data_callback(callbacks, send_data_callback);
    if (nghttp2_session_client_new(&session, callbacks, &client) != 0) {
        close(client.fd);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "failed to create nghttp2 session", 0);
        RETURN_THROWS();
    }
    if (client.bench.recv_stream_window_size > 0) {
        nghttp2_settings_entry iv[1] = {
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, client.bench.recv_stream_window_size},
        };
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);
    } else {
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    }
    if (client.bench.recv_connection_window_size > 65535) {
        nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE, 0, (int32_t) (client.bench.recv_connection_window_size - 65535));
    }

    snprintf(authority, sizeof(authority), "%s:%ld", host, port);
    if (init_request_headers(&request_headers, count_custom_header_values(headers_zv)) != 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        cleanup_grpc_call(&client);
        RETURN_THROWS();
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, "http", sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, authority, strlen(authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        close(client.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&client);
        RETURN_THROWS();
    }

    memset(&data_provider, 0, sizeof(data_provider));
    data_provider.read_callback = bench_data_source_read_callback;
    array_init(&latencies);
    array_init(&client_first_data_sent_us);
    array_init(&client_upload_complete_us);
    array_init(&client_first_response_data_us);
    array_init(&client_last_response_data_us);
    array_init(&client_first_window_update_us);
    array_init(&client_last_window_update_us);
    array_init(&client_first_window_update_sent_us);
    array_init(&client_last_window_update_sent_us);
    array_init(&client_first_flow_control_pause_us);
    array_init(&client_response_header_us);
    array_init(&client_stream_close_us);
    array_init(&client_first_response_message_ready_us);
    array_init(&client_last_response_message_ready_us);
    array_init(&client_first_response_callback_done_us);
    array_init(&client_last_response_callback_done_us);
    array_init(&call_window_update_frames_recv);
    array_init(&call_connection_window_update_frames_recv);
    array_init(&call_stream_window_update_frames_recv);
    array_init(&call_connection_window_update_increment_recv);
    array_init(&call_stream_window_update_increment_recv);
    array_init(&call_window_update_frames_sent);
    array_init(&call_connection_window_update_frames_sent);
    array_init(&call_stream_window_update_frames_sent);
    array_init(&call_connection_window_update_increment_sent);
    array_init(&call_stream_window_update_increment_sent);
    array_init(&call_data_read_length_calls);
    array_init(&call_flow_control_pauses);
    array_init(&call_max_write_syscall_us);
    array_init(&call_recv_syscalls);
    array_init(&call_recv_syscall_us);
    array_init(&call_max_recv_syscall_us);
    array_init(&call_mem_recv_us);
    array_init(&call_max_mem_recv_us);
    array_init(&call_session_send_after_recv_us);
    array_init(&call_max_session_send_after_recv_us);
    array_init(&call_poll_wait_us);
    array_init(&call_max_poll_wait_us);
    array_init(&call_pollin_ready);
    array_init(&call_pollout_ready);
    array_init(&call_poll_to_data_us);
    array_init(&call_max_poll_to_data_us);
    array_init(&call_window_update_to_data_us);
    array_init(&call_max_window_update_to_data_us);
    array_init(&call_receive_drains);
    array_init(&call_receive_drains_with_data);
    array_init(&call_receive_drains_eagain_after_data);
    array_init(&call_max_reads_per_drain);
    array_init(&call_max_bytes_per_drain);
    array_init(&call_min_session_remote_window);
    array_init(&call_min_stream_remote_window);
    array_init(&call_response_data_bytes);
    array_init(&call_data_recv_calls);
    array_init(&call_body_append_us);
    array_init(&call_max_body_append_us);
    array_init(&call_body_compact_count);
    array_init(&call_body_compact_bytes);
    array_init(&call_body_compact_us);
    array_init(&call_max_body_compact_us);
    array_init(&call_max_body_buffer_bytes);
    array_init(&call_decoded_messages);
    array_init(&call_max_response_queue_count);
    array_init(&call_max_response_queue_bytes);
    array_init(&call_response_queue_wait_us);
    array_init(&call_max_response_queue_wait_us);
    array_init(&call_response_payload_string_us);
    array_init(&call_max_response_payload_string_us);
    array_init(&call_response_decode_us);
    array_init(&call_max_response_decode_us);
    array_init(&server_handler_ns);
    array_init(&server_payload_alloc_ns);
    array_init(&server_payload_bytes);
    array_init(&server_request_payload_bytes);
    array_init(&server_stats_handler_start_ns);
    array_init(&server_stats_handler_end_ns);
    array_init(&server_stats_in_payload_ns);
    array_init(&server_stats_out_header_ns);
    array_init(&server_stats_out_payload_ns);
    array_init(&server_stats_first_out_payload_ns);
    array_init(&server_stats_last_out_payload_ns);
    array_init(&server_stats_out_payload_count);
    array_init(&server_stats_out_payload_bytes);
    array_init(&server_stats_out_payload_wire_bytes);
    array_init(&server_stats_out_payload_compressed_bytes);
    total_started = monotonic_us();

    for (zend_long i = 0; i < iterations; i++) {
        uint64_t started = monotonic_us();
        client.bench.call_started_us = started;
        client.deadline_abs_us = timeout_us > 0 ? started + (uint64_t) timeout_us : 0;
        client.stream_closed = false;
        client.grpc_status = -1;
        client.http_status = -1;
        client.stream_error_code = 0;
        client.compressed_response_seen = false;
        client.response_current_compressed = false;
        client.timed_out = false;
        client.request_offset = 0;
        client.pending_data_len = 0;
        clear_pending_write(&client);
        client.bench.first_data_sent_us = 0;
        client.bench.last_data_sent_us = 0;
        client.bench.first_response_data_us = 0;
        client.bench.last_response_data_us = 0;
        client.bench.first_window_update_us = 0;
        client.bench.last_window_update_us = 0;
        client.bench.first_window_update_sent_us = 0;
        client.bench.last_window_update_sent_us = 0;
        client.bench.first_flow_control_pause_us = 0;
        client.bench.first_response_header_us = 0;
        client.bench.stream_closed_us = 0;
        client.bench.first_response_message_ready_us = 0;
        client.bench.last_response_message_ready_us = 0;
        client.bench.first_response_callback_done_us = 0;
        client.bench.last_response_callback_done_us = 0;
        client.bench.call_window_update_frames_recv = 0;
        client.bench.call_connection_window_update_frames_recv = 0;
        client.bench.call_stream_window_update_frames_recv = 0;
        client.bench.call_connection_window_update_increment_recv = 0;
        client.bench.call_stream_window_update_increment_recv = 0;
        client.bench.call_window_update_frames_sent = 0;
        client.bench.call_connection_window_update_frames_sent = 0;
        client.bench.call_stream_window_update_frames_sent = 0;
        client.bench.call_connection_window_update_increment_sent = 0;
        client.bench.call_stream_window_update_increment_sent = 0;
        client.bench.call_data_read_length_calls = 0;
        client.bench.call_flow_control_pauses = 0;
        client.bench.call_max_write_syscall_us = 0;
        client.bench.call_recv_syscalls = 0;
        client.bench.call_recv_syscall_us = 0;
        client.bench.call_max_recv_syscall_us = 0;
        client.bench.call_mem_recv_us = 0;
        client.bench.call_max_mem_recv_us = 0;
        client.bench.call_session_send_after_recv_us = 0;
        client.bench.call_max_session_send_after_recv_us = 0;
        client.bench.call_poll_wait_us = 0;
        client.bench.call_max_poll_wait_us = 0;
        client.bench.call_pollin_ready = 0;
        client.bench.call_pollout_ready = 0;
        client.bench.call_poll_to_data_us = 0;
        client.bench.call_max_poll_to_data_us = 0;
        client.bench.call_window_update_to_data_us = 0;
        client.bench.call_max_window_update_to_data_us = 0;
        client.bench.call_receive_drains = 0;
        client.bench.call_receive_drains_with_data = 0;
        client.bench.call_receive_drains_eagain_after_data = 0;
        client.bench.call_max_reads_per_drain = 0;
        client.bench.call_max_bytes_per_drain = 0;
        client.bench.last_poll_return_abs_us = 0;
        client.bench.awaiting_data_after_poll = false;
        client.bench.last_window_update_sent_abs_us = 0;
        client.bench.awaiting_data_after_window_update_sent = false;
        client.bench.call_min_session_remote_window = 0;
        client.bench.call_min_stream_remote_window = 0;
        client.bench.call_response_data_bytes = 0;
        client.bench.call_data_recv_calls = 0;
        client.bench.call_body_append_us = 0;
        client.bench.call_max_body_append_us = 0;
        client.bench.call_body_compact_count = 0;
        client.bench.call_body_compact_bytes = 0;
        client.bench.call_body_compact_us = 0;
        client.bench.call_max_body_compact_us = 0;
        client.bench.call_max_body_buffer_bytes = 0;
        client.response_parse_offset = 0;
        client.response_header_len = 0;
        client.response_payload_len = 0;
        client.response_payload_offset = 0;
        if (client.response_payload != NULL) {
            zend_string_release(client.response_payload);
            client.response_payload = NULL;
        }
        client.bench.call_decoded_messages = 0;
        client.bench.call_max_response_queue_count = 0;
        client.bench.call_max_response_queue_bytes = 0;
        client.bench.call_response_queue_wait_us = 0;
        client.bench.call_max_response_queue_wait_us = 0;
        free_queued_response_payloads(&client);
        client.bench.call_response_payload_string_us = 0;
        client.bench.call_max_response_payload_string_us = 0;
        client.bench.call_response_decode_us = 0;
        client.bench.call_max_response_decode_us = 0;
        client.bench.server_handler_ns = 0;
        client.bench.server_payload_alloc_ns = 0;
        client.bench.server_payload_bytes = 0;
        client.bench.server_request_payload_bytes = 0;
        client.bench.server_stats_handler_start_ns = 0;
        client.bench.server_stats_handler_end_ns = 0;
        client.bench.server_stats_in_payload_ns = 0;
        client.bench.server_stats_out_header_ns = 0;
        client.bench.server_stats_out_payload_ns = 0;
        client.bench.server_stats_first_out_payload_ns = 0;
        client.bench.server_stats_last_out_payload_ns = 0;
        client.bench.server_stats_out_payload_count = 0;
        client.bench.server_stats_out_payload_bytes = 0;
        client.bench.server_stats_out_payload_wire_bytes = 0;
        client.bench.server_stats_out_payload_compressed_bytes = 0;
        cleanup_grpc_call(&client);
        memset(&client.body, 0, sizeof(client.body));

        client.stream_id = nghttp2_submit_request(session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
        if (client.stream_id < 0) {
            failed++;
            break;
        }

        if (poll_loop) {
            rv = drive_stream_poll(session, &client, recv_buf, recv_buf_len);
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
                ssize_t nread = recv(client.fd, recv_buf, recv_buf_len, 0);
                if (nread <= 0) {
                    if (nread < 0) {
                        client.last_io_errno = errno;
                        snprintf(client.last_io_error_detail, sizeof(client.last_io_error_detail), "recv failed: %s", strerror(errno));
                    }
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

        zend_long decoded_messages = client.bench.call_decoded_messages;
        uint64_t response_payload_string_us = client.bench.call_response_payload_string_us;
        uint64_t max_response_payload_string_us = client.bench.call_max_response_payload_string_us;
        uint64_t response_decode_us = client.bench.call_response_decode_us;
        uint64_t max_response_decode_us = client.bench.call_max_response_decode_us;
        if (response_callback_enabled && !decode_response_incrementally) {
            if (bench_process_response_messages(&client, &response_fci, &response_fcc, &decoded_messages, &response_decode_us, &max_response_decode_us) != 0) {
                failed++;
                if (EG(exception)) {
                    break;
                }
                break;
            }
        } else if (response_callback_enabled && decode_response_incrementally && client.direct_response_payload && (client.response_header_len != 0 || client.response_payload != NULL || client.response_queue_head != NULL)) {
            failed++;
            break;
        } else if (response_callback_enabled && decode_response_incrementally && !client.direct_response_payload && client.response_parse_offset != (client.body.s ? ZSTR_LEN(client.body.s) : 0)) {
            failed++;
            break;
        }

        add_next_index_long(&latencies, (zend_long) (monotonic_us() - started));
        add_next_index_long(&client_first_data_sent_us, (zend_long) client.bench.first_data_sent_us);
        add_next_index_long(&client_upload_complete_us, (zend_long) client.bench.last_data_sent_us);
        add_next_index_long(&client_first_response_data_us, (zend_long) client.bench.first_response_data_us);
        add_next_index_long(&client_last_response_data_us, (zend_long) client.bench.last_response_data_us);
        add_next_index_long(&client_first_window_update_us, (zend_long) client.bench.first_window_update_us);
        add_next_index_long(&client_last_window_update_us, (zend_long) client.bench.last_window_update_us);
        add_next_index_long(&client_first_window_update_sent_us, (zend_long) client.bench.first_window_update_sent_us);
        add_next_index_long(&client_last_window_update_sent_us, (zend_long) client.bench.last_window_update_sent_us);
        add_next_index_long(&client_first_flow_control_pause_us, (zend_long) client.bench.first_flow_control_pause_us);
        add_next_index_long(&client_response_header_us, (zend_long) client.bench.first_response_header_us);
        add_next_index_long(&client_stream_close_us, (zend_long) client.bench.stream_closed_us);
        add_next_index_long(&client_first_response_message_ready_us, (zend_long) client.bench.first_response_message_ready_us);
        add_next_index_long(&client_last_response_message_ready_us, (zend_long) client.bench.last_response_message_ready_us);
        add_next_index_long(&client_first_response_callback_done_us, (zend_long) client.bench.first_response_callback_done_us);
        add_next_index_long(&client_last_response_callback_done_us, (zend_long) client.bench.last_response_callback_done_us);
        add_next_index_long(&call_window_update_frames_recv, (zend_long) client.bench.call_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_frames_recv, (zend_long) client.bench.call_connection_window_update_frames_recv);
        add_next_index_long(&call_stream_window_update_frames_recv, (zend_long) client.bench.call_stream_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_increment_recv, (zend_long) client.bench.call_connection_window_update_increment_recv);
        add_next_index_long(&call_stream_window_update_increment_recv, (zend_long) client.bench.call_stream_window_update_increment_recv);
        add_next_index_long(&call_window_update_frames_sent, (zend_long) client.bench.call_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_frames_sent, (zend_long) client.bench.call_connection_window_update_frames_sent);
        add_next_index_long(&call_stream_window_update_frames_sent, (zend_long) client.bench.call_stream_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_increment_sent, (zend_long) client.bench.call_connection_window_update_increment_sent);
        add_next_index_long(&call_stream_window_update_increment_sent, (zend_long) client.bench.call_stream_window_update_increment_sent);
        add_next_index_long(&call_data_read_length_calls, (zend_long) client.bench.call_data_read_length_calls);
        add_next_index_long(&call_flow_control_pauses, (zend_long) client.bench.call_flow_control_pauses);
        add_next_index_long(&call_max_write_syscall_us, (zend_long) client.bench.call_max_write_syscall_us);
        add_next_index_long(&call_recv_syscalls, (zend_long) client.bench.call_recv_syscalls);
        add_next_index_long(&call_recv_syscall_us, (zend_long) client.bench.call_recv_syscall_us);
        add_next_index_long(&call_max_recv_syscall_us, (zend_long) client.bench.call_max_recv_syscall_us);
        add_next_index_long(&call_mem_recv_us, (zend_long) client.bench.call_mem_recv_us);
        add_next_index_long(&call_max_mem_recv_us, (zend_long) client.bench.call_max_mem_recv_us);
        add_next_index_long(&call_session_send_after_recv_us, (zend_long) client.bench.call_session_send_after_recv_us);
        add_next_index_long(&call_max_session_send_after_recv_us, (zend_long) client.bench.call_max_session_send_after_recv_us);
        add_next_index_long(&call_poll_wait_us, (zend_long) client.bench.call_poll_wait_us);
        add_next_index_long(&call_max_poll_wait_us, (zend_long) client.bench.call_max_poll_wait_us);
        add_next_index_long(&call_pollin_ready, (zend_long) client.bench.call_pollin_ready);
        add_next_index_long(&call_pollout_ready, (zend_long) client.bench.call_pollout_ready);
        add_next_index_long(&call_poll_to_data_us, (zend_long) client.bench.call_poll_to_data_us);
        add_next_index_long(&call_max_poll_to_data_us, (zend_long) client.bench.call_max_poll_to_data_us);
        add_next_index_long(&call_window_update_to_data_us, (zend_long) client.bench.call_window_update_to_data_us);
        add_next_index_long(&call_max_window_update_to_data_us, (zend_long) client.bench.call_max_window_update_to_data_us);
        add_next_index_long(&call_receive_drains, (zend_long) client.bench.call_receive_drains);
        add_next_index_long(&call_receive_drains_with_data, (zend_long) client.bench.call_receive_drains_with_data);
        add_next_index_long(&call_receive_drains_eagain_after_data, (zend_long) client.bench.call_receive_drains_eagain_after_data);
        add_next_index_long(&call_max_reads_per_drain, (zend_long) client.bench.call_max_reads_per_drain);
        add_next_index_long(&call_max_bytes_per_drain, (zend_long) client.bench.call_max_bytes_per_drain);
        add_next_index_long(&call_min_session_remote_window, (zend_long) client.bench.call_min_session_remote_window);
        add_next_index_long(&call_min_stream_remote_window, (zend_long) client.bench.call_min_stream_remote_window);
        add_next_index_long(&call_response_data_bytes, (zend_long) client.bench.call_response_data_bytes);
        add_next_index_long(&call_data_recv_calls, (zend_long) client.bench.call_data_recv_calls);
        add_next_index_long(&call_body_append_us, (zend_long) client.bench.call_body_append_us);
        add_next_index_long(&call_max_body_append_us, (zend_long) client.bench.call_max_body_append_us);
        add_next_index_long(&call_body_compact_count, (zend_long) client.bench.call_body_compact_count);
        add_next_index_long(&call_body_compact_bytes, (zend_long) client.bench.call_body_compact_bytes);
        add_next_index_long(&call_body_compact_us, (zend_long) client.bench.call_body_compact_us);
        add_next_index_long(&call_max_body_compact_us, (zend_long) client.bench.call_max_body_compact_us);
        add_next_index_long(&call_max_body_buffer_bytes, (zend_long) client.bench.call_max_body_buffer_bytes);
        add_next_index_long(&call_decoded_messages, decoded_messages);
        add_next_index_long(&call_max_response_queue_count, (zend_long) client.bench.call_max_response_queue_count);
        add_next_index_long(&call_max_response_queue_bytes, (zend_long) client.bench.call_max_response_queue_bytes);
        add_next_index_long(&call_response_queue_wait_us, (zend_long) client.bench.call_response_queue_wait_us);
        add_next_index_long(&call_max_response_queue_wait_us, (zend_long) client.bench.call_max_response_queue_wait_us);
        add_next_index_long(&call_response_payload_string_us, (zend_long) response_payload_string_us);
        add_next_index_long(&call_max_response_payload_string_us, (zend_long) max_response_payload_string_us);
        add_next_index_long(&call_response_decode_us, (zend_long) response_decode_us);
        add_next_index_long(&call_max_response_decode_us, (zend_long) max_response_decode_us);
        add_next_index_long(&server_handler_ns, client.bench.server_handler_ns);
        add_next_index_long(&server_payload_alloc_ns, client.bench.server_payload_alloc_ns);
        add_next_index_long(&server_payload_bytes, client.bench.server_payload_bytes);
        add_next_index_long(&server_request_payload_bytes, client.bench.server_request_payload_bytes);
        add_next_index_long(&server_stats_handler_start_ns, client.bench.server_stats_handler_start_ns);
        add_next_index_long(&server_stats_handler_end_ns, client.bench.server_stats_handler_end_ns);
        add_next_index_long(&server_stats_in_payload_ns, client.bench.server_stats_in_payload_ns);
        add_next_index_long(&server_stats_out_header_ns, client.bench.server_stats_out_header_ns);
        add_next_index_long(&server_stats_out_payload_ns, client.bench.server_stats_out_payload_ns);
        add_next_index_long(&server_stats_first_out_payload_ns, client.bench.server_stats_first_out_payload_ns);
        add_next_index_long(&server_stats_last_out_payload_ns, client.bench.server_stats_last_out_payload_ns);
        add_next_index_long(&server_stats_out_payload_count, client.bench.server_stats_out_payload_count);
        add_next_index_long(&server_stats_out_payload_bytes, client.bench.server_stats_out_payload_bytes);
        add_next_index_long(&server_stats_out_payload_wire_bytes, client.bench.server_stats_out_payload_wire_bytes);
        add_next_index_long(&server_stats_out_payload_compressed_bytes, client.bench.server_stats_out_payload_compressed_bytes);
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
    free_request_headers(&request_headers);

    array_init(return_value);
    add_assoc_long(return_value, "iterations", iterations);
    add_assoc_long(return_value, "ok", ok);
    add_assoc_long(return_value, "failed", failed);
    add_assoc_long(return_value, "total_us", (zend_long) total_elapsed);
    add_assoc_long(return_value, "grpc_status", client.grpc_status);
    add_assoc_long(return_value, "http_status", client.http_status);
    add_assoc_long(return_value, "stream_error_code", client.stream_error_code);
    add_assoc_long(return_value, "body_bytes", client.body.s ? ZSTR_LEN(client.body.s) : 0);
    add_assoc_bool(return_value, "discard_response_body", discard_response_body);
    add_assoc_bool(return_value, "split_grpc_frame", split_grpc_frame);
    add_assoc_bool(return_value, "no_copy", no_copy);
    add_assoc_bool(return_value, "poll_loop", poll_loop);
    add_assoc_bool(return_value, "flush_after_mem_recv", flush_after_mem_recv);
    add_assoc_bool(return_value, "read_first_poll_loop", read_first_poll_loop);
    add_assoc_bool(return_value, "response_callback_enabled", response_callback_enabled);
    add_assoc_bool(return_value, "decode_response_incrementally", decode_response_incrementally);
    add_assoc_bool(return_value, "direct_response_payload", client.direct_response_payload);
    add_assoc_bool(return_value, "read_ahead_delivery", client.bench.read_ahead_delivery);
    add_assoc_bool(return_value, "timed_out", client.timed_out);
    add_assoc_long(return_value, "last_io_errno", client.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", client.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", client.last_io_error_detail);
    add_assoc_bool(return_value, "compact_response_buffer", client.bench.compact_response_buffer);
    add_assoc_long(return_value, "response_compact_threshold", (zend_long) client.bench.response_compact_threshold);
    add_assoc_long(return_value, "request_wire_bytes", client.grpc_header_len + client.request_len);
    add_assoc_long(return_value, "bytes_sent", client.bytes_sent);
    add_assoc_long(return_value, "bytes_received", client.bytes_received);
    add_assoc_long(return_value, "response_data_bytes", client.bench.response_data_bytes);
    add_assoc_long(return_value, "sent_frames", client.sent_frames);
    add_assoc_long(return_value, "recv_frames", client.recv_frames);
    add_assoc_long(return_value, "data_frames_sent", client.bench.data_frames_sent);
    add_assoc_long(return_value, "data_bytes_sent", client.bench.data_bytes_sent);
    add_assoc_long(return_value, "window_update_frames_recv", client.bench.window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_frames_recv", client.bench.connection_window_update_frames_recv);
    add_assoc_long(return_value, "stream_window_update_frames_recv", client.bench.stream_window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_increment_recv", client.bench.connection_window_update_increment_recv);
    add_assoc_long(return_value, "stream_window_update_increment_recv", client.bench.stream_window_update_increment_recv);
    add_assoc_long(return_value, "window_update_frames_sent", client.bench.window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_frames_sent", client.bench.connection_window_update_frames_sent);
    add_assoc_long(return_value, "stream_window_update_frames_sent", client.bench.stream_window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_increment_sent", client.bench.connection_window_update_increment_sent);
    add_assoc_long(return_value, "stream_window_update_increment_sent", client.bench.stream_window_update_increment_sent);
    add_assoc_long(return_value, "flow_control_pauses", client.bench.flow_control_pauses);
    add_assoc_long(return_value, "send_callback_calls", client.bench.send_callback_calls);
    add_assoc_long(return_value, "send_data_callback_calls", client.bench.send_data_callback_calls);
    add_assoc_long(return_value, "write_syscalls", client.bench.write_syscalls);
    add_assoc_long(return_value, "send_wouldblock_calls", client.bench.send_wouldblock_calls);
    add_assoc_long(return_value, "recv_wouldblock_calls", client.bench.recv_wouldblock_calls);
    add_assoc_long(return_value, "poll_calls", client.bench.poll_calls);
    add_assoc_long(return_value, "poll_timeouts", client.bench.poll_timeouts);
    add_assoc_long(return_value, "poll_errors", client.bench.poll_errors);
    add_assoc_long(return_value, "max_write_syscall_us", client.bench.max_write_syscall_us);
    add_assoc_long(return_value, "data_read_calls", client.data_read_calls);
    add_assoc_long(return_value, "data_read_length_calls", client.bench.data_read_length_calls);
    add_assoc_long(return_value, "data_recv_calls", client.data_recv_calls);
    add_assoc_long(return_value, "max_send_callback_len", client.bench.max_send_callback_len);
    add_assoc_long(return_value, "max_data_frame_len", client.bench.max_data_frame_len);
    add_assoc_long(return_value, "min_data_frame_len", client.bench.min_data_frame_len);
    add_assoc_long(return_value, "max_read_len", client.bench.max_read_len);
    add_assoc_long(return_value, "min_read_len", client.bench.min_read_len);
    add_assoc_long(return_value, "data_frame_size_cap", client.bench.data_frame_size_cap);
    add_assoc_long(return_value, "recv_stream_window_size", client.bench.recv_stream_window_size);
    add_assoc_long(return_value, "recv_connection_window_size", client.bench.recv_connection_window_size);
    add_assoc_long(return_value, "recv_buffer_size", (zend_long) recv_buf_len);
    add_assoc_long(return_value, "min_session_remote_window", client.bench.min_session_remote_window);
    add_assoc_long(return_value, "min_stream_remote_window", client.bench.min_stream_remote_window);
    add_assoc_long(return_value, "remote_max_frame_size", client.bench.remote_max_frame_size);
    add_assoc_zval(return_value, "latencies_us", &latencies);
    add_assoc_zval(return_value, "client_first_data_sent_us", &client_first_data_sent_us);
    add_assoc_zval(return_value, "client_upload_complete_us", &client_upload_complete_us);
    add_assoc_zval(return_value, "client_first_response_data_us", &client_first_response_data_us);
    add_assoc_zval(return_value, "client_last_response_data_us", &client_last_response_data_us);
    add_assoc_zval(return_value, "client_first_window_update_us", &client_first_window_update_us);
    add_assoc_zval(return_value, "client_last_window_update_us", &client_last_window_update_us);
    add_assoc_zval(return_value, "client_first_window_update_sent_us", &client_first_window_update_sent_us);
    add_assoc_zval(return_value, "client_last_window_update_sent_us", &client_last_window_update_sent_us);
    add_assoc_zval(return_value, "client_first_flow_control_pause_us", &client_first_flow_control_pause_us);
    add_assoc_zval(return_value, "client_response_header_us", &client_response_header_us);
    add_assoc_zval(return_value, "client_stream_close_us", &client_stream_close_us);
    add_assoc_zval(return_value, "client_first_response_message_ready_us", &client_first_response_message_ready_us);
    add_assoc_zval(return_value, "client_last_response_message_ready_us", &client_last_response_message_ready_us);
    add_assoc_zval(return_value, "client_first_response_callback_done_us", &client_first_response_callback_done_us);
    add_assoc_zval(return_value, "client_last_response_callback_done_us", &client_last_response_callback_done_us);
    add_assoc_zval(return_value, "call_window_update_frames_recv", &call_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_frames_recv", &call_connection_window_update_frames_recv);
    add_assoc_zval(return_value, "call_stream_window_update_frames_recv", &call_stream_window_update_frames_recv);
    add_assoc_zval(return_value, "call_connection_window_update_increment_recv", &call_connection_window_update_increment_recv);
    add_assoc_zval(return_value, "call_stream_window_update_increment_recv", &call_stream_window_update_increment_recv);
    add_assoc_zval(return_value, "call_window_update_frames_sent", &call_window_update_frames_sent);
    add_assoc_zval(return_value, "call_connection_window_update_frames_sent", &call_connection_window_update_frames_sent);
    add_assoc_zval(return_value, "call_stream_window_update_frames_sent", &call_stream_window_update_frames_sent);
    add_assoc_zval(return_value, "call_connection_window_update_increment_sent", &call_connection_window_update_increment_sent);
    add_assoc_zval(return_value, "call_stream_window_update_increment_sent", &call_stream_window_update_increment_sent);
    add_assoc_zval(return_value, "call_data_read_length_calls", &call_data_read_length_calls);
    add_assoc_zval(return_value, "call_flow_control_pauses", &call_flow_control_pauses);
    add_assoc_zval(return_value, "call_max_write_syscall_us", &call_max_write_syscall_us);
    add_assoc_zval(return_value, "call_recv_syscalls", &call_recv_syscalls);
    add_assoc_zval(return_value, "call_recv_syscall_us", &call_recv_syscall_us);
    add_assoc_zval(return_value, "call_max_recv_syscall_us", &call_max_recv_syscall_us);
    add_assoc_zval(return_value, "call_mem_recv_us", &call_mem_recv_us);
    add_assoc_zval(return_value, "call_max_mem_recv_us", &call_max_mem_recv_us);
    add_assoc_zval(return_value, "call_session_send_after_recv_us", &call_session_send_after_recv_us);
    add_assoc_zval(return_value, "call_max_session_send_after_recv_us", &call_max_session_send_after_recv_us);
    add_assoc_zval(return_value, "call_poll_wait_us", &call_poll_wait_us);
    add_assoc_zval(return_value, "call_max_poll_wait_us", &call_max_poll_wait_us);
    add_assoc_zval(return_value, "call_pollin_ready", &call_pollin_ready);
    add_assoc_zval(return_value, "call_pollout_ready", &call_pollout_ready);
    add_assoc_zval(return_value, "call_poll_to_data_us", &call_poll_to_data_us);
    add_assoc_zval(return_value, "call_max_poll_to_data_us", &call_max_poll_to_data_us);
    add_assoc_zval(return_value, "call_window_update_to_data_us", &call_window_update_to_data_us);
    add_assoc_zval(return_value, "call_max_window_update_to_data_us", &call_max_window_update_to_data_us);
    add_assoc_zval(return_value, "call_receive_drains", &call_receive_drains);
    add_assoc_zval(return_value, "call_receive_drains_with_data", &call_receive_drains_with_data);
    add_assoc_zval(return_value, "call_receive_drains_eagain_after_data", &call_receive_drains_eagain_after_data);
    add_assoc_zval(return_value, "call_max_reads_per_drain", &call_max_reads_per_drain);
    add_assoc_zval(return_value, "call_max_bytes_per_drain", &call_max_bytes_per_drain);
    add_assoc_zval(return_value, "call_min_session_remote_window", &call_min_session_remote_window);
    add_assoc_zval(return_value, "call_min_stream_remote_window", &call_min_stream_remote_window);
    add_assoc_zval(return_value, "call_response_data_bytes", &call_response_data_bytes);
    add_assoc_zval(return_value, "call_data_recv_calls", &call_data_recv_calls);
    add_assoc_zval(return_value, "call_body_append_us", &call_body_append_us);
    add_assoc_zval(return_value, "call_max_body_append_us", &call_max_body_append_us);
    add_assoc_zval(return_value, "call_body_compact_count", &call_body_compact_count);
    add_assoc_zval(return_value, "call_body_compact_bytes", &call_body_compact_bytes);
    add_assoc_zval(return_value, "call_body_compact_us", &call_body_compact_us);
    add_assoc_zval(return_value, "call_max_body_compact_us", &call_max_body_compact_us);
    add_assoc_zval(return_value, "call_max_body_buffer_bytes", &call_max_body_buffer_bytes);
    add_assoc_zval(return_value, "call_decoded_messages", &call_decoded_messages);
    add_assoc_zval(return_value, "call_max_response_queue_count", &call_max_response_queue_count);
    add_assoc_zval(return_value, "call_max_response_queue_bytes", &call_max_response_queue_bytes);
    add_assoc_zval(return_value, "call_response_queue_wait_us", &call_response_queue_wait_us);
    add_assoc_zval(return_value, "call_max_response_queue_wait_us", &call_max_response_queue_wait_us);
    add_assoc_zval(return_value, "call_response_payload_string_us", &call_response_payload_string_us);
    add_assoc_zval(return_value, "call_max_response_payload_string_us", &call_max_response_payload_string_us);
    add_assoc_zval(return_value, "call_response_decode_us", &call_response_decode_us);
    add_assoc_zval(return_value, "call_max_response_decode_us", &call_max_response_decode_us);
    add_assoc_zval(return_value, "server_handler_ns", &server_handler_ns);
    add_assoc_zval(return_value, "server_payload_alloc_ns", &server_payload_alloc_ns);
    add_assoc_zval(return_value, "server_payload_bytes", &server_payload_bytes);
    add_assoc_zval(return_value, "server_request_payload_bytes", &server_request_payload_bytes);
    add_assoc_zval(return_value, "server_stats_handler_start_ns", &server_stats_handler_start_ns);
    add_assoc_zval(return_value, "server_stats_handler_end_ns", &server_stats_handler_end_ns);
    add_assoc_zval(return_value, "server_stats_in_payload_ns", &server_stats_in_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_header_ns", &server_stats_out_header_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_ns", &server_stats_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_first_out_payload_ns", &server_stats_first_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_last_out_payload_ns", &server_stats_last_out_payload_ns);
    add_assoc_zval(return_value, "server_stats_out_payload_count", &server_stats_out_payload_count);
    add_assoc_zval(return_value, "server_stats_out_payload_bytes", &server_stats_out_payload_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_wire_bytes", &server_stats_out_payload_wire_bytes);
    add_assoc_zval(return_value, "server_stats_out_payload_compressed_bytes", &server_stats_out_payload_compressed_bytes);
    cleanup_grpc_call(&client);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_multiplex_unary, 0, 5, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, stream_count, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_bench_unary_batch, 0, 5, IS_ARRAY, 0)
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
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, discard_response_body, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_stream_window_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_connection_window_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, recv_buffer_size, IS_LONG, 0, "16384")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flush_after_mem_recv, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_first_poll_loop, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, response_callback, IS_CALLABLE, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, decode_response_incrementally, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, compact_response_buffer, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, response_compact_threshold, IS_LONG, 0, "1")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, direct_response_payload, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_delivery, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_max_messages, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, read_ahead_max_bytes, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout_us, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

/* Bench-build diagnostic PHP entrypoints for direct transport measurement. */
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

    if (grpc_lite_open_stream_resource(key, key_len, host, host_len, port, path, path_len, request, request_len, headers_zv, timeout_us, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, max_receive_message_length, authority, authority_len, tls_verify_name, tls_verify_name_len, return_value) != SUCCESS) {
        RETURN_THROWS();
    }
}

PHP_FUNCTION(grpc_lite_stream_next)
{
    zval *stream_zv = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();
    if (grpc_lite_stream_next_resource(stream_zv, return_value) != SUCCESS) {
        RETURN_THROWS();
    }
}

PHP_FUNCTION(grpc_lite_stream_cancel)
{
    zval *stream_zv = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();
    if (grpc_lite_cancel_stream_resource(stream_zv) != SUCCESS) {
        RETURN_THROWS();
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
    PHP_FE(grpc_lite_multiplex_unary, arginfo_grpc_lite_multiplex_unary)
    PHP_FE(grpc_lite_bench_unary_batch, arginfo_grpc_lite_bench_unary_batch)
    PHP_FE_END
};
