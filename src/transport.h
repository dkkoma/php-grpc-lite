#ifndef PHP_GRPC_LITE_TRANSPORT_H
#define PHP_GRPC_LITE_TRANSPORT_H

#include "grpc_exchange_state.h"
#include "grpc_result.h"
#include "h2_request_headers.h"
#include "protocol_core.h"
#include "transport_core.h"

struct _h2_connection {
    int fd;
    bool tls;
    bool persistent;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    char authority[GRPC_LITE_AUTHORITY_BUFFER_SIZE];
    bool dead;
    bool draining;
    bool detached_from_cache;
    size_t active_stream_count;
    size_t stream_owner_count;
    grpc_call *active_streams;
    grpc_call *current_io_call;
    grpc_call *current_read_call;
    uint64_t current_write_deadline_abs_us;
    bool current_write_timed_out;
    uint8_t *write_buffer;
    size_t write_buffer_len;
    size_t write_buffer_cap;
    /* True only inside send_pending_h2_frames_with_deadline, which always
     * flushes write_buffer before returning (and marks the connection dead
     * on a failed flush). Any other path driving send_callback must leave
     * this false, or buffered bytes would linger across calls. */
    bool write_coalescing;
    uint8_t *recv_scratch;
    size_t recv_scratch_len;
    uint64_t setup_deadline_abs_us;
    bool setup_timed_out;
    int last_error;
    int last_io_errno;
    int last_ssl_error;
    long tls_verify_result;
    char last_error_detail[256];
    char negotiated_protocol[16];
    uint32_t last_goaway_error_code;
    int32_t last_goaway_stream_id;
};

struct persistent_connection_entry {
    h2_connection *connection;
    zend_string *connection_key_identity;
};

struct server_streaming_call_state {
    grpc_call call;
    zend_string *request;
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
    zend_string *path;
#endif
    uint64_t delivered_messages;
    bool completed;
    bool cancelled;
};

extern int le_server_streaming_call_state;

void destroy_h2_connection(h2_connection *connection);
void destroy_persistent_connection_entry(persistent_connection_entry *entry, bool destroy_connection);
void detach_persistent_connection_by_ptr(h2_connection *connection);
bool connection_owned_by_server_streaming_call_state(h2_connection *connection, server_streaming_call_state *state);
bool connection_owned_by_call(h2_connection *connection, grpc_call *call);
void clear_connection_server_streaming_call_state_owner(server_streaming_call_state *state);
void clear_connection_call_owner(h2_connection *connection, grpc_call *call);
void destroy_detached_connection_if_unowned(h2_connection *connection);
int register_grpc_call_stream(h2_connection *connection, grpc_call *call);
void mark_grpc_call_stream_registration_failed(h2_connection *connection, grpc_call *call);
int send_pending_h2_frames(h2_connection *connection, grpc_call *call);
int send_pending_h2_frames_with_deadline(h2_connection *connection, grpc_call *call, uint64_t fallback_deadline_abs_us);
void cancel_grpc_call_stream(grpc_call *call, uint32_t error_code);
void cancel_active_server_streaming_call_state(server_streaming_call_state *state, uint32_t error_code);
void destroy_server_streaming_call_state(server_streaming_call_state *state);
void server_streaming_call_state_dtor(zend_resource *rsrc);
int configure_callbacks(nghttp2_session_callbacks **callbacks);
void mark_connection_dead(h2_connection *connection, int error_code);
void set_connection_error_detail(h2_connection *connection, const char *detail);
void mark_connection_draining(h2_connection *connection, int32_t last_stream_id, uint32_t error_code);
bool connection_usable(h2_connection *connection);
bool connection_io_allowed(h2_connection *connection);
#ifdef PHP_GRPC_LITE_ENABLE_TEST_FAULT
void grpc_lite_test_fault_init(void);
bool grpc_lite_test_fault_enabled(const char *fault_name);
#else
/* Production builds compile the test fault seams out entirely. */
#define grpc_lite_test_fault_enabled(fault_name) false
#endif
void grpc_call_note_connection_broken(grpc_call *call);
zend_string *grpc_lite_build_connection_key(const char *host, size_t host_len, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, int credentials_type, zend_string *root_certs, zend_string *cert_chain, zend_string *private_key);
persistent_connection_entry *create_persistent_connection_entry(h2_connection *connection, const char *key, size_t key_len);
bool connection_entry_matches_key(persistent_connection_entry *entry, const char *key, size_t key_len);
bool preflight_persistent_connection(h2_connection *connection, uint64_t deadline_abs_us);
void remove_unusable_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
int set_fd_nonblocking_mode(int fd, bool nonblocking);
int poll_timeout_ms_for_deadline(uint64_t deadline_abs_us);
zend_long remaining_timeout_us_for_deadline(uint64_t deadline_abs_us);
int poll_fd_until_deadline(int fd, short events, uint64_t deadline_abs_us);
int configure_tls_connection(h2_connection *connection, const char *host, const char *tls_verify_name, size_t tls_verify_name_len, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us);
ssize_t connection_send(grpc_call *call, const uint8_t *data, size_t length);
ssize_t connection_recv(h2_connection *connection, uint8_t *data, size_t length, uint64_t deadline_abs_us);
h2_connection *create_h2_connection(const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, bool persistent, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, const char **error_message);
h2_connection *get_persistent_connection(const char *key, size_t key_len, const char *host, zend_long port, const char *authority, size_t authority_len, const char *tls_verify_name, size_t tls_verify_name_len, bool use_tls, const char *root_certs, size_t root_certs_len, const char *cert_chain, size_t cert_chain_len, const char *private_key, size_t private_key_len, uint64_t deadline_abs_us, char *error_detail, size_t error_detail_len, bool *persistent_reused, const char **error_message);
void discard_persistent_connection(const char *key, size_t key_len, h2_connection *connection);
int connect_tcp(const char *host, zend_long port, uint64_t deadline_abs_us);
ssize_t send_callback(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data);
size_t remaining_request_bytes(grpc_call *call);
ssize_t data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
int h2_send_data_callback(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd, size_t length, nghttp2_data_source *source, void *user_data);
void grpc_protocol_set_message_header(grpc_call *call, size_t payload_len);
int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data);
int on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
int on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
int on_invalid_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data);
int on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data);
int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
uint64_t monotonic_us(void);
void grpc_lite_trace_cache_init(void);
void grpc_lite_trace_cache_shutdown(void);
const char *grpc_lite_trace_file_path(void);
uint8_t *h2_connection_recv_scratch(h2_connection *connection);
#ifdef PHP_GRPC_LITE_ENABLE_BENCH
zend_long header_value_to_long(const uint8_t *value, size_t valuelen);
#endif
zend_string *grpc_protocol_decode_message(const uint8_t *value, size_t valuelen);
int grpc_lite_status_code_from_call(grpc_call *call, bool cancelled);
bool grpc_lite_call_response_started(grpc_call *call);
void grpc_lite_attempt_outcome_from_call(grpc_call *call, bool userland_response_observed, grpc_lite_attempt_outcome *outcome);
int grpc_protocol_validate_response_message_lengths(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
int grpc_protocol_process_response_data_direct(nghttp2_session *session, grpc_call *call, const uint8_t *data, size_t len);
int enqueue_response_payload(nghttp2_session *session, grpc_call *call, zend_string *payload);
void free_queued_response_payloads(grpc_call *call);
void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call);
int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing);
void grpc_protocol_free_response_metadata_entries(grpc_call *call);
void grpc_protocol_copy_metadata_map(zval *metadata, grpc_call *call, bool trailing);
void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing);
void resolve_grpc_call_status(grpc_call *call, bool cancelled, grpc_lite_status_result *result);
void add_status_result_to_return(zval *return_value, grpc_lite_status_result *status);
void cleanup_grpc_call(grpc_call *call);

#endif /* PHP_GRPC_LITE_TRANSPORT_H */
