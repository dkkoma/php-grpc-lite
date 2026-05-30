#include "../surface.h"
#include "diagnostic.h"

/*
 * Bench-only HTTP/2 transport entrypoints.
 *
 * config.m4 compiles this file only for --enable-grpc-bench builds. The
 * benchmark harness uses the same low-level HTTP/2 helpers and diagnostics as
 * the production HTTP/2 transport, but those helpers should remain file-local
 * instead of becoming a wider extension ABI.
 */

#define MAKE_NV(NAME, VALUE) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, sizeof(VALUE) - 1, NGHTTP2_NV_FLAG_NONE}
#define MAKE_NV_L(NAME, VALUE, VALUE_LEN) {(uint8_t *)(NAME), (uint8_t *)(VALUE), sizeof(NAME) - 1, (VALUE_LEN), NGHTTP2_NV_FLAG_NONE}
#define GRPC_BENCH_MAX_RECV_BUF_SIZE 262144

static int bench_process_response_messages_from_offset(grpc_call *call, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us);

static void bench_observe_response_callback_done(grpc_call *call)
{
    if (call->bench.call_started_us == 0) {
        return;
    }
    uint64_t done_us = monotonic_us() - call->bench.call_started_us;
    if (call->bench.first_response_callback_done_us == 0) {
        call->bench.first_response_callback_done_us = done_us;
    }
    call->bench.last_response_callback_done_us = done_us;
}

static void bench_record_data_sent(grpc_call *call)
{
    uint64_t elapsed = monotonic_us() - call->bench.call_started_us;
    if (call->bench.first_data_sent_us == 0) {
        call->bench.first_data_sent_us = elapsed;
    }
    call->bench.last_data_sent_us = elapsed;
}

static void bench_compact_response_body_if_needed(grpc_call *call)
{
    zend_string *body;
    size_t len;
    size_t consumed;
    size_t remaining;
    uint64_t started;
    uint64_t elapsed;

    if (!call->bench.compact_response_buffer || call->body.s == NULL || call->response_parse_offset == 0) {
        return;
    }
    if (call->response_parse_offset < call->bench.response_compact_threshold) {
        return;
    }

    body = call->body.s;
    len = ZSTR_LEN(body);
    consumed = call->response_parse_offset;
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

    call->response_parse_offset = 0;
    call->bench.call_body_compact_count++;
    call->bench.call_body_compact_bytes += consumed;
    call->bench.call_body_compact_us += elapsed;
    if (elapsed > call->bench.call_max_body_compact_us) {
        call->bench.call_max_body_compact_us = elapsed;
    }
}

static int bench_process_response_messages(grpc_call *call, zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_long *decoded_messages, uint64_t *decode_us, uint64_t *max_decode_us)
{
    size_t offset = 0;
    uint64_t payload_string_us = 0;
    uint64_t max_payload_string_us = 0;
    return bench_process_response_messages_from_offset(call, fci, fcc, &offset, true, decoded_messages, &payload_string_us, &max_payload_string_us, decode_us, max_decode_us);
}

static int bench_process_response_messages_from_offset(grpc_call *call, zend_fcall_info *fci, zend_fcall_info_cache *fcc, size_t *offset, bool require_complete, zend_long *decoded_messages, uint64_t *payload_string_us, uint64_t *max_payload_string_us, uint64_t *decode_us, uint64_t *max_decode_us)
{
    zend_string *body;
    const char *data;
    size_t len;

    *decoded_messages = 0;
    *payload_string_us = 0;
    *max_payload_string_us = 0;
    *decode_us = 0;
    *max_decode_us = 0;

    if (call->body.s == NULL) {
        return 0;
    }

    smart_str_0(&call->body);
    body = call->body.s;
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
        bench_observe_response_callback_done(call);
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
    grpc_call *call = (grpc_call *) user_data;
    size_t total_written = 0;
    (void) session;
    (void) flags;

    if (call == NULL) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    call->bench.send_callback_calls++;
    if (length > call->bench.max_send_callback_len) {
        call->bench.max_send_callback_len = length;
    }

    if (call->bench.poll_loop) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = connection_send(call, data, length);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > call->bench.max_write_syscall_us) {
            call->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > call->bench.call_max_write_syscall_us) {
            call->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                call->bench.last_send_wouldblock = true;
                call->bench.send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            call->bench.last_send_wouldblock = true;
            call->bench.send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        call->bench.write_syscalls++;
        call->bytes_sent += (size_t) written;
        return written;
    }

    while (total_written < length) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = connection_send(call, data + total_written, length - total_written);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > call->bench.max_write_syscall_us) {
            call->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > call->bench.call_max_write_syscall_us) {
            call->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        call->bench.write_syscalls++;
        total_written += (size_t) written;
    }
    call->bytes_sent += total_written;
    return (ssize_t) total_written;
}

static int bench_on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    (void) session;
    (void) flags;
    if (stream_id == call->stream_id && len > 0) {
        uint64_t elapsed = call->bench.call_started_us == 0 ? 0 : monotonic_us() - call->bench.call_started_us;
        call->data_recv_calls++;
        call->bench.call_data_recv_calls++;
        call->bench.response_data_bytes += len;
        call->bench.call_response_data_bytes += len;
        if (elapsed > 0) {
            if (call->bench.first_response_data_us == 0) {
                call->bench.first_response_data_us = elapsed;
            }
            call->bench.last_response_data_us = elapsed;
        }
        if (call->bench.awaiting_data_after_poll && call->bench.last_poll_return_abs_us > 0) {
            uint64_t delta = monotonic_us() - call->bench.last_poll_return_abs_us;
            call->bench.call_poll_to_data_us += delta;
            if (delta > call->bench.call_max_poll_to_data_us) {
                call->bench.call_max_poll_to_data_us = delta;
            }
            call->bench.awaiting_data_after_poll = false;
        }
        if (call->bench.awaiting_data_after_window_update_sent && call->bench.last_window_update_sent_abs_us > 0) {
            uint64_t delta = monotonic_us() - call->bench.last_window_update_sent_abs_us;
            call->bench.call_window_update_to_data_us += delta;
            if (delta > call->bench.call_max_window_update_to_data_us) {
                call->bench.call_max_window_update_to_data_us = delta;
            }
            call->bench.awaiting_data_after_window_update_sent = false;
        }
        if (call->direct_response_payload && call->decode_response_incrementally && call->queue_response_payloads) {
            if (grpc_protocol_process_response_data_direct(session, call, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
        } else if (!call->discard_response_body) {
            if (grpc_protocol_validate_response_message_lengths(session, call, data, len) != 0) {
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            if (call->discard_response_body) {
                return 0;
            }
            uint64_t append_started = monotonic_us();
            uint64_t append_elapsed;
            smart_str_appendl(&call->body, (const char *) data, len);
            append_elapsed = monotonic_us() - append_started;
            if (call->body.s != NULL && ZSTR_LEN(call->body.s) > call->bench.call_max_body_buffer_bytes) {
                call->bench.call_max_body_buffer_bytes = ZSTR_LEN(call->body.s);
            }
            call->bench.call_body_append_us += append_elapsed;
            if (append_elapsed > call->bench.call_max_body_append_us) {
                call->bench.call_max_body_append_us = append_elapsed;
            }
        }
    }
    return 0;
}

static int bench_on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    bool trailing;
    (void) session;
    (void) flags;
    if (frame->hd.stream_id != call->stream_id) {
        return 0;
    }
    trailing = frame->headers.cat != NGHTTP2_HCAT_RESPONSE;
    if (namelen == sizeof("grpc-status") - 1 && memcmp(name, "grpc-status", namelen) == 0) {
        call->grpc_status = grpc_protocol_parse_status_value(value, valuelen);
        if (call->grpc_status < 0) {
            call->invalid_grpc_status = true;
        }
        trailing = true;
    } else if (namelen == sizeof("grpc-message") - 1 && memcmp(name, "grpc-message", namelen) == 0) {
        if (call->grpc_message != NULL) {
            zend_string_release(call->grpc_message);
        }
        call->grpc_message = grpc_protocol_decode_message(value, valuelen);
        trailing = true;
    } else if (namelen == sizeof(":status") - 1 && memcmp(name, ":status", namelen) == 0) {
        if (call->bench.first_response_header_us == 0) {
            call->bench.first_response_header_us = monotonic_us() - call->bench.call_started_us;
        }
        char status_buf[16];
        size_t copy_len = valuelen < sizeof(status_buf) - 1 ? valuelen : sizeof(status_buf) - 1;
        memcpy(status_buf, value, copy_len);
        status_buf[copy_len] = '\0';
        call->http_status = atoi(status_buf);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    } else if (namelen == sizeof("x-bench-server-handler-ns") - 1 && memcmp(name, "x-bench-server-handler-ns", namelen) == 0) {
        call->bench.server_handler_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-alloc-ns") - 1 && memcmp(name, "x-bench-server-payload-alloc-ns", namelen) == 0) {
        call->bench.server_payload_alloc_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-payload-bytes") - 1 && memcmp(name, "x-bench-server-payload-bytes", namelen) == 0) {
        call->bench.server_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-request-payload-bytes") - 1 && memcmp(name, "x-bench-server-request-payload-bytes", namelen) == 0) {
        call->bench.server_request_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-start-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-start-ns", namelen) == 0) {
        call->bench.server_stats_handler_start_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-handler-end-ns") - 1 && memcmp(name, "x-bench-server-stats-handler-end-ns", namelen) == 0) {
        call->bench.server_stats_handler_end_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-in-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-in-payload-ns", namelen) == 0) {
        call->bench.server_stats_in_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-header-ns") - 1 && memcmp(name, "x-bench-server-stats-out-header-ns", namelen) == 0) {
        call->bench.server_stats_out_header_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-out-payload-ns", namelen) == 0) {
        call->bench.server_stats_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-first-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-first-out-payload-ns", namelen) == 0) {
        call->bench.server_stats_first_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-last-out-payload-ns") - 1 && memcmp(name, "x-bench-server-stats-last-out-payload-ns", namelen) == 0) {
        call->bench.server_stats_last_out_payload_ns = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-count") - 1 && memcmp(name, "x-bench-server-stats-out-payload-count", namelen) == 0) {
        call->bench.server_stats_out_payload_count = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-bytes", namelen) == 0) {
        call->bench.server_stats_out_payload_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-wire-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-wire-bytes", namelen) == 0) {
        call->bench.server_stats_out_payload_wire_bytes = header_value_to_long(value, valuelen);
    } else if (namelen == sizeof("x-bench-server-stats-out-payload-compressed-bytes") - 1 && memcmp(name, "x-bench-server-stats-out-payload-compressed-bytes", namelen) == 0) {
        call->bench.server_stats_out_payload_compressed_bytes = header_value_to_long(value, valuelen);
#endif
    }
    if (grpc_protocol_add_response_metadata_entry(call, name, namelen, value, valuelen, trailing) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int bench_on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    (void) session;
    (void) error_code;
    if (stream_id == call->stream_id) {
        call->stream_closed = true;
        call->stream_error_code = error_code;
        call->bench.stream_closed_us = monotonic_us() - call->bench.call_started_us;
    }
    return 0;
}

static int bench_on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    (void) session;
    call->sent_frames++;
    call->last_sent_frame_type = frame->hd.type;
    call->last_sent_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_DATA && !call->bench.no_copy) {
        call->bench.data_frames_sent++;
        call->bench.data_bytes_sent += frame->hd.length;
        bench_record_data_sent(call);
        if (frame->hd.length > call->bench.max_data_frame_len) {
            call->bench.max_data_frame_len = frame->hd.length;
        }
        if (call->bench.min_data_frame_len == 0 || frame->hd.length < call->bench.min_data_frame_len) {
            call->bench.min_data_frame_len = frame->hd.length;
        }
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = call->bench.call_started_us == 0 ? 0 : monotonic_us() - call->bench.call_started_us;
        call->bench.window_update_frames_sent++;
        call->bench.call_window_update_frames_sent++;
        if (frame->hd.stream_id == 0) {
            call->bench.connection_window_update_frames_sent++;
            call->bench.call_connection_window_update_frames_sent++;
            call->bench.connection_window_update_increment_sent += frame->window_update.window_size_increment;
            call->bench.call_connection_window_update_increment_sent += frame->window_update.window_size_increment;
        } else {
            call->bench.stream_window_update_frames_sent++;
            call->bench.call_stream_window_update_frames_sent++;
            call->bench.stream_window_update_increment_sent += frame->window_update.window_size_increment;
            call->bench.call_stream_window_update_increment_sent += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (call->bench.first_window_update_sent_us == 0) {
                call->bench.first_window_update_sent_us = elapsed;
            }
            call->bench.last_window_update_sent_us = elapsed;
            call->bench.last_window_update_sent_abs_us = monotonic_us();
            call->bench.awaiting_data_after_window_update_sent = true;
        }
    }
    return 0;
}

static int bench_on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    (void) session;
    call->recv_frames++;
    call->last_recv_frame_type = frame->hd.type;
    call->last_recv_frame_flags = frame->hd.flags;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        mark_connection_draining(call->connection, frame->goaway.last_stream_id, frame->goaway.error_code);
    } else if (frame->hd.type == NGHTTP2_WINDOW_UPDATE) {
        uint64_t elapsed = call->bench.call_started_us == 0 ? 0 : monotonic_us() - call->bench.call_started_us;
        call->bench.window_update_frames_recv++;
        call->bench.call_window_update_frames_recv++;
        if (frame->hd.stream_id == 0) {
            call->bench.connection_window_update_frames_recv++;
            call->bench.call_connection_window_update_frames_recv++;
            call->bench.connection_window_update_increment_recv += frame->window_update.window_size_increment;
            call->bench.call_connection_window_update_increment_recv += frame->window_update.window_size_increment;
        } else {
            call->bench.stream_window_update_frames_recv++;
            call->bench.call_stream_window_update_frames_recv++;
            call->bench.stream_window_update_increment_recv += frame->window_update.window_size_increment;
            call->bench.call_stream_window_update_increment_recv += frame->window_update.window_size_increment;
        }
        if (elapsed > 0) {
            if (call->bench.first_window_update_us == 0) {
                call->bench.first_window_update_us = elapsed;
            }
            call->bench.last_window_update_us = elapsed;
        }
    }
    return 0;
}

static int bench_on_frame_not_send_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    (void) session;
    call->not_sent_frames++;
    call->last_not_sent_frame_type = frame->hd.type;
    call->last_not_sent_error = lib_error_code;
    return 0;
}


static ssize_t data_source_read_length_callback(nghttp2_session *session, uint8_t frame_type, int32_t stream_id, int32_t session_remote_window_size, int32_t stream_remote_window_size, uint32_t remote_max_frame_size, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
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
    if (call->bench.data_frame_size_cap > 0 && call->bench.data_frame_size_cap < allowed) {
        allowed = call->bench.data_frame_size_cap;
    }
    if (allowed == 0) {
        uint64_t elapsed = call->bench.call_started_us == 0 ? 0 : monotonic_us() - call->bench.call_started_us;
        call->bench.flow_control_pauses++;
        call->bench.call_flow_control_pauses++;
        if (elapsed > 0 && call->bench.first_flow_control_pause_us == 0) {
            call->bench.first_flow_control_pause_us = elapsed;
        }
        return NGHTTP2_ERR_PAUSE;
    }

    call->bench.data_read_length_calls++;
    call->bench.call_data_read_length_calls++;
    call->bench.remote_max_frame_size = remote_max_frame_size;
    if (call->bench.min_session_remote_window == 0 || session_remote_window_size < call->bench.min_session_remote_window) {
        call->bench.min_session_remote_window = session_remote_window_size;
    }
    if (call->bench.min_stream_remote_window == 0 || stream_remote_window_size < call->bench.min_stream_remote_window) {
        call->bench.min_stream_remote_window = stream_remote_window_size;
    }
    if (call->bench.call_min_session_remote_window == 0 || session_remote_window_size < call->bench.call_min_session_remote_window) {
        call->bench.call_min_session_remote_window = session_remote_window_size;
    }
    if (call->bench.call_min_stream_remote_window == 0 || stream_remote_window_size < call->bench.call_min_stream_remote_window) {
        call->bench.call_min_stream_remote_window = stream_remote_window_size;
    }
    if (allowed > call->bench.max_read_len) {
        call->bench.max_read_len = allowed;
    }
    if (call->bench.min_read_len == 0 || allowed < call->bench.min_read_len) {
        call->bench.min_read_len = allowed;
    }

    return (ssize_t) allowed;
}

static size_t fill_request_iov(grpc_call *call, struct iovec *iov, size_t iov_offset, size_t length, size_t *filled_len)
{
    size_t filled = 0;
    size_t total_len = call->grpc_header_len + call->request_len;
    size_t request_offset = call->request_offset;

    while (filled < length && request_offset < total_len && iov_offset < 4) {
        if (request_offset < call->grpc_header_len) {
            size_t header_offset = request_offset;
            size_t remaining = call->grpc_header_len - header_offset;
            size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
            iov[iov_offset].iov_base = call->grpc_header + header_offset;
            iov[iov_offset].iov_len = to_write;
            iov_offset++;
            filled += to_write;
            request_offset += to_write;
            continue;
        }

        size_t payload_offset = request_offset - call->grpc_header_len;
        size_t remaining = call->request_len - payload_offset;
        size_t to_write = remaining < (length - filled) ? remaining : (length - filled);
        iov[iov_offset].iov_base = (void *) (call->request + payload_offset);
        iov[iov_offset].iov_len = to_write;
        iov_offset++;
        filled += to_write;
        request_offset += to_write;
    }

    *filled_len = filled;
    return iov_offset;
}

static int write_data_frame(grpc_call *call, const uint8_t *framehd, size_t length)
{
    struct iovec iov[4];
    size_t iovcnt = 1;
    size_t filled_len = 0;
    size_t total_len = 9 + length;
    size_t total_written = 0;

    iov[0].iov_base = (void *) framehd;
    iov[0].iov_len = 9;
    iovcnt = fill_request_iov(call, iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        return -1;
    }

    while (total_written < total_len) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(call->fd, iov, (int) iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > call->bench.max_write_syscall_us) {
            call->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > call->bench.call_max_write_syscall_us) {
            call->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written <= 0) {
            return -1;
        }
        call->bench.write_syscalls++;
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

    call->request_offset += length;
    call->bytes_sent += total_len;
    return 0;
}

static void clear_pending_write(grpc_call *call)
{
    call->pending_write_iovcnt = 0;
    call->pending_write_remaining = 0;
    call->pending_write_payload_len = 0;
}

static void consume_pending_write(grpc_call *call, size_t consumed)
{
    while (call->pending_write_iovcnt > 0 && consumed > 0) {
        struct iovec *iov = &call->pending_write_iov[0];
        if (consumed >= iov->iov_len) {
            consumed -= iov->iov_len;
            call->pending_write_remaining -= iov->iov_len;
            memmove(&call->pending_write_iov[0], &call->pending_write_iov[1], sizeof(struct iovec) * (call->pending_write_iovcnt - 1));
            call->pending_write_iovcnt--;
            continue;
        }

        iov->iov_base = (uint8_t *) iov->iov_base + consumed;
        iov->iov_len -= consumed;
        call->pending_write_remaining -= consumed;
        consumed = 0;
    }
}

static int prepare_pending_data_frame_write(grpc_call *call, const uint8_t *framehd, size_t length)
{
    size_t iovcnt = 1;
    size_t filled_len = 0;

    call->pending_write_iov[0].iov_base = (void *) framehd;
    call->pending_write_iov[0].iov_len = 9;
    iovcnt = fill_request_iov(call, call->pending_write_iov, iovcnt, length, &filled_len);
    if (filled_len != length) {
        clear_pending_write(call);
        return -1;
    }

    call->pending_write_iovcnt = iovcnt;
    call->pending_write_remaining = 9 + length;
    call->pending_write_payload_len = length;
    return 0;
}

static int flush_pending_data_frame_write(grpc_call *call)
{
    while (call->pending_write_remaining > 0) {
        uint64_t syscall_started = monotonic_us();
        ssize_t written = writev(call->fd, call->pending_write_iov, (int) call->pending_write_iovcnt);
        uint64_t syscall_elapsed = monotonic_us() - syscall_started;
        if (syscall_elapsed > call->bench.max_write_syscall_us) {
            call->bench.max_write_syscall_us = syscall_elapsed;
        }
        if (syscall_elapsed > call->bench.call_max_write_syscall_us) {
            call->bench.call_max_write_syscall_us = syscall_elapsed;
        }
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                call->bench.last_send_wouldblock = true;
                call->bench.send_wouldblock_calls++;
                return NGHTTP2_ERR_WOULDBLOCK;
            }
            clear_pending_write(call);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (written == 0) {
            call->bench.last_send_wouldblock = true;
            call->bench.send_wouldblock_calls++;
            return NGHTTP2_ERR_WOULDBLOCK;
        }

        call->bench.write_syscalls++;
        call->bytes_sent += (size_t) written;
        consume_pending_write(call, (size_t) written);
    }

    call->request_offset += call->pending_write_payload_len;
    clear_pending_write(call);
    return 0;
}

static int write_data_frame_nonblocking(grpc_call *call, const uint8_t *framehd, size_t length)
{
    if (call->pending_write_remaining == 0 && prepare_pending_data_frame_write(call, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return flush_pending_data_frame_write(call);
}

static int send_data_callback(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    bool new_data_frame = call->pending_write_remaining == 0;
    (void) session;
    (void) frame;
    (void) source;

    call->bench.send_data_callback_calls++;
    if (new_data_frame) {
        call->bench.data_frames_sent++;
        call->bench.data_bytes_sent += length;
        if (length > call->bench.max_data_frame_len) {
            call->bench.max_data_frame_len = length;
        }
        if (call->bench.min_data_frame_len == 0 || length < call->bench.min_data_frame_len) {
            call->bench.min_data_frame_len = length;
        }
    }

    if (call->bench.poll_loop) {
        if (write_data_frame(call, framehd, length) != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    } else if (write_data_frame(call, framehd, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    bench_record_data_sent(call);
    call->pending_data_len = 0;

    return 0;
}

static ssize_t bench_data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    grpc_call *call = (grpc_call *) user_data;
    size_t total_len = call->grpc_header_len + call->request_len;
    size_t remaining = remaining_request_bytes(call);
    size_t to_send = remaining < length ? remaining : length;
    (void) session;
    (void) stream_id;
    (void) source;

    *data_flags = 0;
    call->data_read_calls++;

    if (call->bench.no_copy && to_send > 0) {
        call->pending_data_len = to_send;
        *data_flags = NGHTTP2_DATA_FLAG_NO_COPY;
        if (call->request_offset + to_send >= total_len) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return (ssize_t) to_send;
    }

    size_t copied = copy_request_bytes(call, buf, to_send);
    if (call->request_offset >= total_len) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t) copied;
}

static int receive_available(nghttp2_session *session, grpc_call *call, char *recv_buf, size_t recv_buf_len)
{
    size_t reads = 0;
    size_t bytes = 0;
    call->bench.call_receive_drains++;
    for (;;) {
        uint64_t recv_started = monotonic_us();
        ssize_t nread = recv(call->fd, recv_buf, recv_buf_len, 0);
        uint64_t recv_elapsed = monotonic_us() - recv_started;
        if (nread > 0) {
            int rv;
            uint64_t mem_recv_started;
            uint64_t mem_recv_elapsed;
            call->bench.call_recv_syscalls++;
            call->bench.call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > call->bench.call_max_recv_syscall_us) {
                call->bench.call_max_recv_syscall_us = recv_elapsed;
            }
            reads++;
            bytes += (size_t) nread;
            call->bytes_received += (size_t) nread;
            mem_recv_started = monotonic_us();
            rv = nghttp2_session_mem_recv(session, (const uint8_t *) recv_buf, (size_t) nread);
            mem_recv_elapsed = monotonic_us() - mem_recv_started;
            call->bench.call_mem_recv_us += mem_recv_elapsed;
            if (mem_recv_elapsed > call->bench.call_max_mem_recv_us) {
                call->bench.call_max_mem_recv_us = mem_recv_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
            if (call->bench.flush_after_mem_recv && nghttp2_session_want_write(session)) {
                uint64_t send_started = monotonic_us();
                uint64_t send_elapsed;
                rv = nghttp2_session_send(session);
                send_elapsed = monotonic_us() - send_started;
                call->bench.call_session_send_after_recv_us += send_elapsed;
                if (send_elapsed > call->bench.call_max_session_send_after_recv_us) {
                    call->bench.call_max_session_send_after_recv_us = send_elapsed;
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
            call->bench.call_recv_syscalls++;
            call->bench.call_recv_syscall_us += recv_elapsed;
            if (recv_elapsed > call->bench.call_max_recv_syscall_us) {
                call->bench.call_max_recv_syscall_us = recv_elapsed;
            }
            call->bench.recv_wouldblock_calls++;
            if (reads > 0) {
                call->bench.call_receive_drains_with_data++;
                call->bench.call_receive_drains_eagain_after_data++;
                if (reads > call->bench.call_max_reads_per_drain) {
                    call->bench.call_max_reads_per_drain = reads;
                }
                if (bytes > call->bench.call_max_bytes_per_drain) {
                    call->bench.call_max_bytes_per_drain = bytes;
                }
            }
            return 0;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
}

static int drive_stream_poll(nghttp2_session *session, grpc_call *call, char *recv_buf, size_t recv_buf_len)
{
    while (!call->stream_closed && (nghttp2_session_want_read(session) || nghttp2_session_want_write(session))) {
        int rv;

        if (call->deadline_abs_us > 0 && monotonic_us() >= call->deadline_abs_us) {
            call->timed_out = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        if (call->bench.read_first_poll_loop && nghttp2_session_want_read(session)) {
            rv = receive_available(session, call, recv_buf, recv_buf_len);
            if (rv < 0) {
                return rv;
            }
            if (call->stream_closed) {
                return 0;
            }
        }

        do {
            uint64_t send_started;
            uint64_t send_elapsed;
            call->bench.last_send_wouldblock = false;
            send_started = monotonic_us();
            rv = nghttp2_session_send(session);
            send_elapsed = monotonic_us() - send_started;
            call->bench.call_session_send_after_recv_us += send_elapsed;
            if (send_elapsed > call->bench.call_max_session_send_after_recv_us) {
                call->bench.call_max_session_send_after_recv_us = send_elapsed;
            }
            if (rv < 0) {
                return rv;
            }
        } while (!call->bench.last_send_wouldblock && nghttp2_session_want_write(session));

        rv = receive_available(session, call, recv_buf, recv_buf_len);
        if (rv < 0) {
            return rv;
        }
        if (call->stream_closed) {
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
        pfd.fd = call->fd;
        pfd.events = events;
        pfd.revents = 0;
        call->bench.poll_calls++;
        uint64_t poll_started = monotonic_us();
        int poll_timeout_ms = 5000;
        if (call->deadline_abs_us > 0) {
            uint64_t remaining_us;
            if (poll_started >= call->deadline_abs_us) {
                call->timed_out = true;
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            remaining_us = call->deadline_abs_us - poll_started;
            poll_timeout_ms = (int) ((remaining_us + 999) / 1000);
            if (poll_timeout_ms < 1) {
                poll_timeout_ms = 1;
            } else if (poll_timeout_ms > 5000) {
                poll_timeout_ms = 5000;
            }
        }
        rv = poll(&pfd, 1, poll_timeout_ms);
        uint64_t poll_elapsed = monotonic_us() - poll_started;
        call->bench.call_poll_wait_us += poll_elapsed;
        if (poll_elapsed > call->bench.call_max_poll_wait_us) {
            call->bench.call_max_poll_wait_us = poll_elapsed;
        }
        if (rv == 0) {
            call->bench.poll_timeouts++;
            if (call->deadline_abs_us > 0 && monotonic_us() >= call->deadline_abs_us) {
                call->timed_out = true;
            }
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            call->bench.poll_errors++;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        if ((pfd.revents & POLLIN) != 0) {
            call->bench.call_pollin_ready++;
            call->bench.last_poll_return_abs_us = monotonic_us();
            call->bench.awaiting_data_after_poll = true;
        }
        if ((pfd.revents & POLLOUT) != 0) {
            call->bench.call_pollout_ready++;
        }
    }

    return call->stream_closed ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
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

    snprintf(authority, sizeof(authority), "%s:%ld", host, (long) port);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":method", "POST");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV(":scheme", "http");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":authority", authority, strlen(authority));
    nva[nvlen++] = (nghttp2_nv) MAKE_NV_L(":path", path, path_len);
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("content-type", "application/grpc");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("te", "trailers");
    nva[nvlen++] = (nghttp2_nv) MAKE_NV("user-agent", PHP_GRPC_LITE_BENCH_USER_AGENT);

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
    grpc_call call;
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
    memset(&call, 0, sizeof(call));
    call.fd = -1;
    call.grpc_status = -1;
    call.http_status = -1;
    call.request = (const uint8_t *) request;
    call.request_len = request_len;
    call.bench.no_copy = no_copy;
    call.bench.poll_loop = poll_loop;
    call.discard_response_body = discard_response_body;
    call.bench.discard_response_body = discard_response_body;
    call.bench.flush_after_mem_recv = flush_after_mem_recv;
    call.bench.read_first_poll_loop = read_first_poll_loop;
    call.decode_response_incrementally = decode_response_incrementally;
    call.direct_response_payload = direct_response_payload && decode_response_incrementally && read_ahead_delivery;
    call.bench.read_ahead_delivery = read_ahead_delivery && call.direct_response_payload;
    call.queue_response_payloads = call.bench.read_ahead_delivery;
    call.bench.read_ahead_max_messages = read_ahead_max_messages > 0 ? (size_t) read_ahead_max_messages : 0;
    call.bench.read_ahead_max_bytes = read_ahead_max_bytes > 0 ? (size_t) read_ahead_max_bytes : 0;
    call.bench.compact_response_buffer = compact_response_buffer && decode_response_incrementally && !call.direct_response_payload;
    call.bench.response_compact_threshold = response_compact_threshold > 0 ? (size_t) response_compact_threshold : 1;
    if (data_frame_size > 0) {
        call.bench.data_frame_size_cap = (uint32_t) data_frame_size;
    }
    if (recv_stream_window_size > 0) {
        call.bench.recv_stream_window_size = (uint32_t) recv_stream_window_size;
    }
    if (recv_connection_window_size > 0) {
        call.bench.recv_connection_window_size = (uint32_t) recv_connection_window_size;
    }
    if (recv_buffer_size > 0) {
        recv_buf_len = (size_t) recv_buffer_size;
        if (recv_buf_len > GRPC_BENCH_MAX_RECV_BUF_SIZE) {
            recv_buf_len = GRPC_BENCH_MAX_RECV_BUF_SIZE;
        }
    }
    if (split_grpc_frame) {
        grpc_protocol_set_message_header(&call, request_len);
    }

    call.deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    call.fd = connect_tcp(host, port, call.deadline_abs_us);
    if (call.fd < 0) {
        zend_throw_exception(NULL, errno == ETIMEDOUT ? "HTTP/2 transport deadline exceeded" : "failed to connect", 0);
        RETURN_THROWS();
    }
    if (poll_loop && set_fd_nonblocking_mode(call.fd, true) != 0) {
        close(call.fd);
        zend_throw_exception(NULL, "failed to set nonblocking", 0);
        RETURN_THROWS();
    }

    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        close(call.fd);
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
    if (nghttp2_session_client_new(&session, callbacks, &call) != 0) {
        close(call.fd);
        nghttp2_session_callbacks_del(callbacks);
        zend_throw_exception(NULL, "failed to create nghttp2 session", 0);
        RETURN_THROWS();
    }
    if (call.bench.recv_stream_window_size > 0) {
        nghttp2_settings_entry iv[1] = {
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, call.bench.recv_stream_window_size},
        };
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);
    } else {
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    }
    if (call.bench.recv_connection_window_size > 65535) {
        nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE, 0, (int32_t) (call.bench.recv_connection_window_size - 65535));
    }

    snprintf(authority, sizeof(authority), "%s:%ld", host, (long) port);
    if (init_request_headers(&request_headers) != 0) {
        close(call.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        cleanup_grpc_call(&call);
        RETURN_THROWS();
    }
    append_request_header(&request_headers, ":method", sizeof(":method") - 1, "POST", sizeof("POST") - 1);
    append_request_header(&request_headers, ":scheme", sizeof(":scheme") - 1, "http", sizeof("http") - 1);
    append_request_header(&request_headers, ":authority", sizeof(":authority") - 1, authority, strlen(authority));
    append_request_header(&request_headers, ":path", sizeof(":path") - 1, path, path_len);
    append_request_header(&request_headers, "content-type", sizeof("content-type") - 1, "application/grpc", sizeof("application/grpc") - 1);
    append_request_header(&request_headers, "te", sizeof("te") - 1, "trailers", sizeof("trailers") - 1);
    if (append_custom_request_headers(&request_headers, headers_zv) != 0) {
        close(call.fd);
        nghttp2_session_del(session);
        nghttp2_session_callbacks_del(callbacks);
        free_request_headers(&request_headers);
        cleanup_grpc_call(&call);
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
        call.bench.call_started_us = started;
        call.deadline_abs_us = timeout_us > 0 ? started + (uint64_t) timeout_us : 0;
        call.stream_closed = false;
        call.grpc_status = -1;
        call.http_status = -1;
        call.stream_error_code = 0;
        call.compressed_response_seen = false;
        call.response_current_compressed = false;
        call.timed_out = false;
        call.request_offset = 0;
        call.pending_data_len = 0;
        clear_pending_write(&call);
        call.bench.first_data_sent_us = 0;
        call.bench.last_data_sent_us = 0;
        call.bench.first_response_data_us = 0;
        call.bench.last_response_data_us = 0;
        call.bench.first_window_update_us = 0;
        call.bench.last_window_update_us = 0;
        call.bench.first_window_update_sent_us = 0;
        call.bench.last_window_update_sent_us = 0;
        call.bench.first_flow_control_pause_us = 0;
        call.bench.first_response_header_us = 0;
        call.bench.stream_closed_us = 0;
        call.bench.first_response_callback_done_us = 0;
        call.bench.last_response_callback_done_us = 0;
        call.bench.call_window_update_frames_recv = 0;
        call.bench.call_connection_window_update_frames_recv = 0;
        call.bench.call_stream_window_update_frames_recv = 0;
        call.bench.call_connection_window_update_increment_recv = 0;
        call.bench.call_stream_window_update_increment_recv = 0;
        call.bench.call_window_update_frames_sent = 0;
        call.bench.call_connection_window_update_frames_sent = 0;
        call.bench.call_stream_window_update_frames_sent = 0;
        call.bench.call_connection_window_update_increment_sent = 0;
        call.bench.call_stream_window_update_increment_sent = 0;
        call.bench.call_data_read_length_calls = 0;
        call.bench.call_flow_control_pauses = 0;
        call.bench.call_max_write_syscall_us = 0;
        call.bench.call_recv_syscalls = 0;
        call.bench.call_recv_syscall_us = 0;
        call.bench.call_max_recv_syscall_us = 0;
        call.bench.call_mem_recv_us = 0;
        call.bench.call_max_mem_recv_us = 0;
        call.bench.call_session_send_after_recv_us = 0;
        call.bench.call_max_session_send_after_recv_us = 0;
        call.bench.call_poll_wait_us = 0;
        call.bench.call_max_poll_wait_us = 0;
        call.bench.call_pollin_ready = 0;
        call.bench.call_pollout_ready = 0;
        call.bench.call_poll_to_data_us = 0;
        call.bench.call_max_poll_to_data_us = 0;
        call.bench.call_window_update_to_data_us = 0;
        call.bench.call_max_window_update_to_data_us = 0;
        call.bench.call_receive_drains = 0;
        call.bench.call_receive_drains_with_data = 0;
        call.bench.call_receive_drains_eagain_after_data = 0;
        call.bench.call_max_reads_per_drain = 0;
        call.bench.call_max_bytes_per_drain = 0;
        call.bench.last_poll_return_abs_us = 0;
        call.bench.awaiting_data_after_poll = false;
        call.bench.last_window_update_sent_abs_us = 0;
        call.bench.awaiting_data_after_window_update_sent = false;
        call.bench.call_min_session_remote_window = 0;
        call.bench.call_min_stream_remote_window = 0;
        call.bench.call_response_data_bytes = 0;
        call.bench.call_data_recv_calls = 0;
        call.bench.call_body_append_us = 0;
        call.bench.call_max_body_append_us = 0;
        call.bench.call_body_compact_count = 0;
        call.bench.call_body_compact_bytes = 0;
        call.bench.call_body_compact_us = 0;
        call.bench.call_max_body_compact_us = 0;
        call.bench.call_max_body_buffer_bytes = 0;
        call.response_parse_offset = 0;
        call.response_header_len = 0;
        call.response_payload_len = 0;
        call.response_payload_offset = 0;
        if (call.response_payload != NULL) {
            zend_string_release(call.response_payload);
            call.response_payload = NULL;
        }
        call.bench.call_decoded_messages = 0;
        call.bench.call_max_response_queue_count = 0;
        call.bench.call_max_response_queue_bytes = 0;
        free_queued_response_payloads(&call);
        call.bench.server_handler_ns = 0;
        call.bench.server_payload_alloc_ns = 0;
        call.bench.server_payload_bytes = 0;
        call.bench.server_request_payload_bytes = 0;
        call.bench.server_stats_handler_start_ns = 0;
        call.bench.server_stats_handler_end_ns = 0;
        call.bench.server_stats_in_payload_ns = 0;
        call.bench.server_stats_out_header_ns = 0;
        call.bench.server_stats_out_payload_ns = 0;
        call.bench.server_stats_first_out_payload_ns = 0;
        call.bench.server_stats_last_out_payload_ns = 0;
        call.bench.server_stats_out_payload_count = 0;
        call.bench.server_stats_out_payload_bytes = 0;
        call.bench.server_stats_out_payload_wire_bytes = 0;
        call.bench.server_stats_out_payload_compressed_bytes = 0;
        cleanup_grpc_call(&call);
        memset(&call.body, 0, sizeof(call.body));

        call.stream_id = nghttp2_submit_request(session, NULL, request_headers.nva, request_headers.len, &data_provider, NULL);
        if (call.stream_id < 0) {
            failed++;
            break;
        }

        if (poll_loop) {
            rv = drive_stream_poll(session, &call, recv_buf, recv_buf_len);
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

            while (!call.stream_closed) {
                ssize_t nread = recv(call.fd, recv_buf, recv_buf_len, 0);
                if (nread <= 0) {
                    if (nread < 0) {
                        call.last_io_errno = errno;
                        snprintf(call.last_io_error_detail, sizeof(call.last_io_error_detail), "recv failed: %s", strerror(errno));
                    }
                    failed++;
                    break;
                }
                call.bytes_received += (size_t) nread;
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
                call.last_session_error = rv;
            }
        }

        zend_long decoded_messages = call.bench.call_decoded_messages;
        uint64_t response_decode_us = 0;
        uint64_t max_response_decode_us = 0;
        if (response_callback_enabled && !decode_response_incrementally) {
            if (bench_process_response_messages(&call, &response_fci, &response_fcc, &decoded_messages, &response_decode_us, &max_response_decode_us) != 0) {
                failed++;
                if (EG(exception)) {
                    break;
                }
                break;
            }
        } else if (response_callback_enabled && decode_response_incrementally && call.direct_response_payload && (call.response_header_len != 0 || call.response_payload != NULL || call.response_queue_head != NULL)) {
            failed++;
            break;
        } else if (response_callback_enabled && decode_response_incrementally && !call.direct_response_payload && call.response_parse_offset != (call.body.s ? ZSTR_LEN(call.body.s) : 0)) {
            failed++;
            break;
        }

        add_next_index_long(&latencies, (zend_long) (monotonic_us() - started));
        add_next_index_long(&client_first_data_sent_us, (zend_long) call.bench.first_data_sent_us);
        add_next_index_long(&client_upload_complete_us, (zend_long) call.bench.last_data_sent_us);
        add_next_index_long(&client_first_response_data_us, (zend_long) call.bench.first_response_data_us);
        add_next_index_long(&client_last_response_data_us, (zend_long) call.bench.last_response_data_us);
        add_next_index_long(&client_first_window_update_us, (zend_long) call.bench.first_window_update_us);
        add_next_index_long(&client_last_window_update_us, (zend_long) call.bench.last_window_update_us);
        add_next_index_long(&client_first_window_update_sent_us, (zend_long) call.bench.first_window_update_sent_us);
        add_next_index_long(&client_last_window_update_sent_us, (zend_long) call.bench.last_window_update_sent_us);
        add_next_index_long(&client_first_flow_control_pause_us, (zend_long) call.bench.first_flow_control_pause_us);
        add_next_index_long(&client_response_header_us, (zend_long) call.bench.first_response_header_us);
        add_next_index_long(&client_stream_close_us, (zend_long) call.bench.stream_closed_us);
        add_next_index_long(&client_first_response_callback_done_us, (zend_long) call.bench.first_response_callback_done_us);
        add_next_index_long(&client_last_response_callback_done_us, (zend_long) call.bench.last_response_callback_done_us);
        add_next_index_long(&call_window_update_frames_recv, (zend_long) call.bench.call_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_frames_recv, (zend_long) call.bench.call_connection_window_update_frames_recv);
        add_next_index_long(&call_stream_window_update_frames_recv, (zend_long) call.bench.call_stream_window_update_frames_recv);
        add_next_index_long(&call_connection_window_update_increment_recv, (zend_long) call.bench.call_connection_window_update_increment_recv);
        add_next_index_long(&call_stream_window_update_increment_recv, (zend_long) call.bench.call_stream_window_update_increment_recv);
        add_next_index_long(&call_window_update_frames_sent, (zend_long) call.bench.call_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_frames_sent, (zend_long) call.bench.call_connection_window_update_frames_sent);
        add_next_index_long(&call_stream_window_update_frames_sent, (zend_long) call.bench.call_stream_window_update_frames_sent);
        add_next_index_long(&call_connection_window_update_increment_sent, (zend_long) call.bench.call_connection_window_update_increment_sent);
        add_next_index_long(&call_stream_window_update_increment_sent, (zend_long) call.bench.call_stream_window_update_increment_sent);
        add_next_index_long(&call_data_read_length_calls, (zend_long) call.bench.call_data_read_length_calls);
        add_next_index_long(&call_flow_control_pauses, (zend_long) call.bench.call_flow_control_pauses);
        add_next_index_long(&call_max_write_syscall_us, (zend_long) call.bench.call_max_write_syscall_us);
        add_next_index_long(&call_recv_syscalls, (zend_long) call.bench.call_recv_syscalls);
        add_next_index_long(&call_recv_syscall_us, (zend_long) call.bench.call_recv_syscall_us);
        add_next_index_long(&call_max_recv_syscall_us, (zend_long) call.bench.call_max_recv_syscall_us);
        add_next_index_long(&call_mem_recv_us, (zend_long) call.bench.call_mem_recv_us);
        add_next_index_long(&call_max_mem_recv_us, (zend_long) call.bench.call_max_mem_recv_us);
        add_next_index_long(&call_session_send_after_recv_us, (zend_long) call.bench.call_session_send_after_recv_us);
        add_next_index_long(&call_max_session_send_after_recv_us, (zend_long) call.bench.call_max_session_send_after_recv_us);
        add_next_index_long(&call_poll_wait_us, (zend_long) call.bench.call_poll_wait_us);
        add_next_index_long(&call_max_poll_wait_us, (zend_long) call.bench.call_max_poll_wait_us);
        add_next_index_long(&call_pollin_ready, (zend_long) call.bench.call_pollin_ready);
        add_next_index_long(&call_pollout_ready, (zend_long) call.bench.call_pollout_ready);
        add_next_index_long(&call_poll_to_data_us, (zend_long) call.bench.call_poll_to_data_us);
        add_next_index_long(&call_max_poll_to_data_us, (zend_long) call.bench.call_max_poll_to_data_us);
        add_next_index_long(&call_window_update_to_data_us, (zend_long) call.bench.call_window_update_to_data_us);
        add_next_index_long(&call_max_window_update_to_data_us, (zend_long) call.bench.call_max_window_update_to_data_us);
        add_next_index_long(&call_receive_drains, (zend_long) call.bench.call_receive_drains);
        add_next_index_long(&call_receive_drains_with_data, (zend_long) call.bench.call_receive_drains_with_data);
        add_next_index_long(&call_receive_drains_eagain_after_data, (zend_long) call.bench.call_receive_drains_eagain_after_data);
        add_next_index_long(&call_max_reads_per_drain, (zend_long) call.bench.call_max_reads_per_drain);
        add_next_index_long(&call_max_bytes_per_drain, (zend_long) call.bench.call_max_bytes_per_drain);
        add_next_index_long(&call_min_session_remote_window, (zend_long) call.bench.call_min_session_remote_window);
        add_next_index_long(&call_min_stream_remote_window, (zend_long) call.bench.call_min_stream_remote_window);
        add_next_index_long(&call_response_data_bytes, (zend_long) call.bench.call_response_data_bytes);
        add_next_index_long(&call_data_recv_calls, (zend_long) call.bench.call_data_recv_calls);
        add_next_index_long(&call_body_append_us, (zend_long) call.bench.call_body_append_us);
        add_next_index_long(&call_max_body_append_us, (zend_long) call.bench.call_max_body_append_us);
        add_next_index_long(&call_body_compact_count, (zend_long) call.bench.call_body_compact_count);
        add_next_index_long(&call_body_compact_bytes, (zend_long) call.bench.call_body_compact_bytes);
        add_next_index_long(&call_body_compact_us, (zend_long) call.bench.call_body_compact_us);
        add_next_index_long(&call_max_body_compact_us, (zend_long) call.bench.call_max_body_compact_us);
        add_next_index_long(&call_max_body_buffer_bytes, (zend_long) call.bench.call_max_body_buffer_bytes);
        add_next_index_long(&call_decoded_messages, decoded_messages);
        add_next_index_long(&call_max_response_queue_count, (zend_long) call.bench.call_max_response_queue_count);
        add_next_index_long(&call_max_response_queue_bytes, (zend_long) call.bench.call_max_response_queue_bytes);
        add_next_index_long(&server_handler_ns, call.bench.server_handler_ns);
        add_next_index_long(&server_payload_alloc_ns, call.bench.server_payload_alloc_ns);
        add_next_index_long(&server_payload_bytes, call.bench.server_payload_bytes);
        add_next_index_long(&server_request_payload_bytes, call.bench.server_request_payload_bytes);
        add_next_index_long(&server_stats_handler_start_ns, call.bench.server_stats_handler_start_ns);
        add_next_index_long(&server_stats_handler_end_ns, call.bench.server_stats_handler_end_ns);
        add_next_index_long(&server_stats_in_payload_ns, call.bench.server_stats_in_payload_ns);
        add_next_index_long(&server_stats_out_header_ns, call.bench.server_stats_out_header_ns);
        add_next_index_long(&server_stats_out_payload_ns, call.bench.server_stats_out_payload_ns);
        add_next_index_long(&server_stats_first_out_payload_ns, call.bench.server_stats_first_out_payload_ns);
        add_next_index_long(&server_stats_last_out_payload_ns, call.bench.server_stats_last_out_payload_ns);
        add_next_index_long(&server_stats_out_payload_count, call.bench.server_stats_out_payload_count);
        add_next_index_long(&server_stats_out_payload_bytes, call.bench.server_stats_out_payload_bytes);
        add_next_index_long(&server_stats_out_payload_wire_bytes, call.bench.server_stats_out_payload_wire_bytes);
        add_next_index_long(&server_stats_out_payload_compressed_bytes, call.bench.server_stats_out_payload_compressed_bytes);
        if (call.stream_closed && call.grpc_status == 0 && call.http_status == 200) {
            ok++;
        } else {
            failed++;
            break;
        }
    }

    total_elapsed = monotonic_us() - total_started;

    close(call.fd);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);
    free_request_headers(&request_headers);

    array_init(return_value);
    add_assoc_long(return_value, "iterations", iterations);
    add_assoc_long(return_value, "ok", ok);
    add_assoc_long(return_value, "failed", failed);
    add_assoc_long(return_value, "total_us", (zend_long) total_elapsed);
    add_assoc_long(return_value, "grpc_status", call.grpc_status);
    add_assoc_long(return_value, "http_status", call.http_status);
    add_assoc_long(return_value, "stream_error_code", call.stream_error_code);
    add_assoc_long(return_value, "body_bytes", call.body.s ? ZSTR_LEN(call.body.s) : 0);
    add_assoc_bool(return_value, "discard_response_body", discard_response_body);
    add_assoc_bool(return_value, "split_grpc_frame", split_grpc_frame);
    add_assoc_bool(return_value, "no_copy", no_copy);
    add_assoc_bool(return_value, "poll_loop", poll_loop);
    add_assoc_bool(return_value, "flush_after_mem_recv", flush_after_mem_recv);
    add_assoc_bool(return_value, "read_first_poll_loop", read_first_poll_loop);
    add_assoc_bool(return_value, "response_callback_enabled", response_callback_enabled);
    add_assoc_bool(return_value, "decode_response_incrementally", decode_response_incrementally);
    add_assoc_bool(return_value, "direct_response_payload", call.direct_response_payload);
    add_assoc_bool(return_value, "read_ahead_delivery", call.bench.read_ahead_delivery);
    add_assoc_bool(return_value, "timed_out", call.timed_out);
    add_assoc_long(return_value, "last_io_errno", call.last_io_errno);
    add_assoc_long(return_value, "last_ssl_error", call.last_ssl_error);
    add_assoc_string(return_value, "last_io_error_detail", call.last_io_error_detail);
    add_assoc_bool(return_value, "compact_response_buffer", call.bench.compact_response_buffer);
    add_assoc_long(return_value, "response_compact_threshold", (zend_long) call.bench.response_compact_threshold);
    add_assoc_long(return_value, "request_wire_bytes", call.grpc_header_len + call.request_len);
    add_assoc_long(return_value, "bytes_sent", call.bytes_sent);
    add_assoc_long(return_value, "bytes_received", call.bytes_received);
    add_assoc_long(return_value, "response_data_bytes", call.bench.response_data_bytes);
    add_assoc_long(return_value, "sent_frames", call.sent_frames);
    add_assoc_long(return_value, "recv_frames", call.recv_frames);
    add_assoc_long(return_value, "data_frames_sent", call.bench.data_frames_sent);
    add_assoc_long(return_value, "data_bytes_sent", call.bench.data_bytes_sent);
    add_assoc_long(return_value, "window_update_frames_recv", call.bench.window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_frames_recv", call.bench.connection_window_update_frames_recv);
    add_assoc_long(return_value, "stream_window_update_frames_recv", call.bench.stream_window_update_frames_recv);
    add_assoc_long(return_value, "connection_window_update_increment_recv", call.bench.connection_window_update_increment_recv);
    add_assoc_long(return_value, "stream_window_update_increment_recv", call.bench.stream_window_update_increment_recv);
    add_assoc_long(return_value, "window_update_frames_sent", call.bench.window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_frames_sent", call.bench.connection_window_update_frames_sent);
    add_assoc_long(return_value, "stream_window_update_frames_sent", call.bench.stream_window_update_frames_sent);
    add_assoc_long(return_value, "connection_window_update_increment_sent", call.bench.connection_window_update_increment_sent);
    add_assoc_long(return_value, "stream_window_update_increment_sent", call.bench.stream_window_update_increment_sent);
    add_assoc_long(return_value, "flow_control_pauses", call.bench.flow_control_pauses);
    add_assoc_long(return_value, "send_callback_calls", call.bench.send_callback_calls);
    add_assoc_long(return_value, "send_data_callback_calls", call.bench.send_data_callback_calls);
    add_assoc_long(return_value, "write_syscalls", call.bench.write_syscalls);
    add_assoc_long(return_value, "send_wouldblock_calls", call.bench.send_wouldblock_calls);
    add_assoc_long(return_value, "recv_wouldblock_calls", call.bench.recv_wouldblock_calls);
    add_assoc_long(return_value, "poll_calls", call.bench.poll_calls);
    add_assoc_long(return_value, "poll_timeouts", call.bench.poll_timeouts);
    add_assoc_long(return_value, "poll_errors", call.bench.poll_errors);
    add_assoc_long(return_value, "max_write_syscall_us", call.bench.max_write_syscall_us);
    add_assoc_long(return_value, "data_read_calls", call.data_read_calls);
    add_assoc_long(return_value, "data_read_length_calls", call.bench.data_read_length_calls);
    add_assoc_long(return_value, "data_recv_calls", call.data_recv_calls);
    add_assoc_long(return_value, "max_send_callback_len", call.bench.max_send_callback_len);
    add_assoc_long(return_value, "max_data_frame_len", call.bench.max_data_frame_len);
    add_assoc_long(return_value, "min_data_frame_len", call.bench.min_data_frame_len);
    add_assoc_long(return_value, "max_read_len", call.bench.max_read_len);
    add_assoc_long(return_value, "min_read_len", call.bench.min_read_len);
    add_assoc_long(return_value, "data_frame_size_cap", call.bench.data_frame_size_cap);
    add_assoc_long(return_value, "recv_stream_window_size", call.bench.recv_stream_window_size);
    add_assoc_long(return_value, "recv_connection_window_size", call.bench.recv_connection_window_size);
    add_assoc_long(return_value, "recv_buffer_size", (zend_long) recv_buf_len);
    add_assoc_long(return_value, "min_session_remote_window", call.bench.min_session_remote_window);
    add_assoc_long(return_value, "min_stream_remote_window", call.bench.min_stream_remote_window);
    add_assoc_long(return_value, "remote_max_frame_size", call.bench.remote_max_frame_size);
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
    cleanup_grpc_call(&call);
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
static zend_string *grpc_lite_build_bench_connection_key(const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len)
{
    zend_string *root_certs_string = root_certs != NULL ? zend_string_init(root_certs, root_certs_len, 0) : NULL;
    zend_string *cert_chain_string = cert_chain != NULL ? zend_string_init(cert_chain, cert_chain_len, 0) : NULL;
    zend_string *private_key_string = private_key != NULL ? zend_string_init(private_key, private_key_len, 0) : NULL;
    zend_string *connection_key = grpc_lite_build_connection_key(
        host,
        host_len,
        port,
        authority,
        authority_len,
        tls_verify_name,
        tls_verify_name_len,
        use_tls ? GRPC_LITE_CREDENTIALS_SSL : GRPC_LITE_CREDENTIALS_INSECURE,
        root_certs_string,
        cert_chain_string,
        private_key_string);

    if (root_certs_string != NULL) zend_string_release(root_certs_string);
    if (cert_chain_string != NULL) zend_string_release(cert_chain_string);
    if (private_key_string != NULL) zend_string_release(private_key_string);
    return connection_key;
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
    h2_connection *connection;
    bool persistent_reused = false;
    const char *error_message = NULL;
    char error_detail[256] = {0};
    uint64_t deadline_abs_us = 0;
    zend_long remaining_timeout_us = 0;
    zend_string *connection_key = NULL;

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

    (void) key;
    (void) key_len;
    error_message = validate_channel_inputs("bench", sizeof("bench") - 1, host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len);
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

    if (!PHP_GRPC_LITE_G(persistent_connections_initialized)) {
        zend_throw_exception(NULL, "persistent connection cache is not initialized", 0);
        RETURN_THROWS();
    }

    connection_key = grpc_lite_build_bench_connection_key(host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len);
    if (connection_key == NULL) {
        zend_throw_exception(NULL, "failed to build grpc_lite connection key", 0);
        RETURN_THROWS();
    }

    deadline_abs_us = timeout_us > 0 ? monotonic_us() + (uint64_t) timeout_us : 0;
    connection = get_persistent_connection(ZSTR_VAL(connection_key), ZSTR_LEN(connection_key), host, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, deadline_abs_us, error_detail, sizeof(error_detail), &persistent_reused, &error_message);
    if (connection == NULL) {
        zend_string_release(connection_key);
        zend_throw_exception(NULL, error_message != NULL ? error_message : "failed to open persistent connection", 0);
        RETURN_THROWS();
    }

    remaining_timeout_us = remaining_timeout_us_for_deadline(deadline_abs_us);
    if (remaining_timeout_us < 0) {
        zend_string_release(connection_key);
        zend_throw_exception(NULL, "HTTP/2 transport deadline exceeded", 0);
        RETURN_THROWS();
    }

    if (grpc_lite_unary_call_perform_diagnostic_on_connection(connection, path, path_len, request, request_len, headers_zv, NULL, remaining_timeout_us, max_receive_message_length, effective_max_response_metadata_bytes(-1, -1), true, persistent_reused, return_value) != SUCCESS) {
        if (connection != NULL && !connection_usable(connection)) {
            remove_unusable_persistent_connection(ZSTR_VAL(connection_key), ZSTR_LEN(connection_key), connection);
        }
        zend_string_release(connection_key);
        RETURN_THROWS();
    }

    if (!connection_usable(connection)) {
        remove_unusable_persistent_connection(ZSTR_VAL(connection_key), ZSTR_LEN(connection_key), connection);
    }
    zend_string_release(connection_key);
}

PHP_FUNCTION(grpc_lite_server_streaming_open)
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
    zend_string *connection_key = NULL;

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

    (void) key;
    (void) key_len;
    connection_key = grpc_lite_build_bench_connection_key(host, host_len, port, authority, authority_len, tls_verify_name, tls_verify_name_len, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len);
    if (connection_key == NULL) {
        zend_throw_exception(NULL, "failed to build grpc_lite connection key", 0);
        RETURN_THROWS();
    }

    if (server_streaming_call_open_resource(ZSTR_VAL(connection_key), ZSTR_LEN(connection_key), host, host_len, port, path, path_len, request, request_len, headers_zv, NULL, timeout_us, use_tls, root_certs, root_certs_len, cert_chain, cert_chain_len, private_key, private_key_len, max_receive_message_length, effective_max_response_metadata_bytes(-1, -1), authority, authority_len, tls_verify_name, tls_verify_name_len, return_value, NULL) != SUCCESS) {
        zend_string_release(connection_key);
        RETURN_THROWS();
    }
    zend_string_release(connection_key);
}

PHP_FUNCTION(grpc_lite_server_streaming_next)
{
    zval *stream_zv = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();
    if (server_streaming_call_next_resource_diagnostic(stream_zv, return_value) != SUCCESS) {
        RETURN_THROWS();
    }
}

PHP_FUNCTION(grpc_lite_server_streaming_cancel)
{
    zval *stream_zv = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(stream_zv)
    ZEND_PARSE_PARAMETERS_END();
    if (server_streaming_call_cancel_resource(stream_zv) != SUCCESS) {
        RETURN_THROWS();
    }
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

ZEND_BEGIN_ARG_INFO_EX(arginfo_grpc_lite_server_streaming_open, 0, 0, 5)
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_server_streaming_next, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_grpc_lite_server_streaming_cancel, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

const zend_function_entry grpc_lite_functions[] = {
    PHP_FE(grpc_lite_unary, arginfo_grpc_lite_unary)
    PHP_FE(grpc_lite_server_streaming_open, arginfo_grpc_lite_server_streaming_open)
    PHP_FE(grpc_lite_server_streaming_next, arginfo_grpc_lite_server_streaming_next)
    PHP_FE(grpc_lite_server_streaming_cancel, arginfo_grpc_lite_server_streaming_cancel)
    PHP_FE(grpc_lite_multiplex_unary, arginfo_grpc_lite_multiplex_unary)
    PHP_FE(grpc_lite_bench_unary_batch, arginfo_grpc_lite_bench_unary_batch)
    PHP_FE_END
};
