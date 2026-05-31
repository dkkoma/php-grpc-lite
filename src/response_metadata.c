#include "response_metadata.h"

#include "metadata_key.h"
#include "transport.h"

#include <ext/standard/base64.h>

int grpc_protocol_add_response_metadata_entry(grpc_call *call, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, bool trailing)
{
    metadata_entry *entry;
    size_t entry_bytes = namelen + valuelen;

    if (namelen == 0 || name[0] == ':') {
        return 0;
    }
    if (call->metadata_too_large
        || call->metadata_entry_count >= GRPC_LITE_MAX_RESPONSE_METADATA_ENTRIES
        || entry_bytes > call->max_response_metadata_bytes
        || call->metadata_bytes > call->max_response_metadata_bytes - entry_bytes) {
        call->metadata_too_large = true;
        call->discard_response_body = true;
        if (call->stream_id > 0) {
            nghttp2_session *session = call->connection != NULL ? call->connection->session : NULL;
            if (session != NULL) {
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, call->stream_id, NGHTTP2_CANCEL);
            }
        }
        return 0;
    }

    entry = emalloc(sizeof(metadata_entry));
    entry->key = zend_string_init((const char *) name, namelen, 0);
    entry->value = zend_string_init((const char *) value, valuelen, 0);
    entry->trailing = trailing;
    entry->next = NULL;

    if (call->metadata_tail == NULL) {
        call->metadata_head = entry;
    } else {
        call->metadata_tail->next = entry;
    }
    call->metadata_tail = entry;
    call->metadata_entry_count++;
    call->metadata_bytes += entry_bytes;

    return 0;
}

void grpc_protocol_mark_response_metadata_as_trailing(grpc_call *call)
{
    metadata_entry *entry;
    for (entry = call->metadata_head; entry != NULL; entry = entry->next) {
        entry->trailing = true;
    }
}

void grpc_protocol_free_response_metadata_entries(grpc_call *call)
{
    while (call->metadata_head != NULL) {
        metadata_entry *entry = call->metadata_head;
        call->metadata_head = entry->next;
        zend_string_release(entry->key);
        zend_string_release(entry->value);
        efree(entry);
    }
    call->metadata_tail = NULL;
}

static void add_metadata_value_to_map(zval *metadata, zend_string *key, zend_string *value)
{
    zval *values = zend_hash_find(Z_ARRVAL_P(metadata), key);
    if (values == NULL) {
        zval new_values;
        array_init(&new_values);
        add_next_index_str(&new_values, value);
        zend_hash_update(Z_ARRVAL_P(metadata), key, &new_values);
    } else if (Z_TYPE_P(values) == IS_ARRAY) {
        add_next_index_str(values, value);
    } else {
        zend_string_release(value);
    }
}

static void add_binary_metadata_values_to_map(zval *metadata, zend_string *key, zend_string *wire_value)
{
    const char *bytes = ZSTR_VAL(wire_value);
    size_t length = ZSTR_LEN(wire_value);
    size_t start = 0;

    while (start <= length) {
        size_t end = start;
        zend_string *value;

        while (end < length && bytes[end] != ',') {
            end++;
        }
        value = php_base64_decode((const unsigned char *) bytes + start, end - start);
        if (value == NULL) {
            value = zend_string_init(bytes + start, end - start, 0);
        }
        add_metadata_value_to_map(metadata, key, value);
        if (end == length) {
            break;
        }
        start = end + 1;
    }
}

void grpc_protocol_copy_metadata_map(zval *metadata, grpc_call *call, bool trailing)
{
    metadata_entry *entry;

    array_init(metadata);
    for (entry = call->metadata_head; entry != NULL; entry = entry->next) {
        if (entry->trailing != trailing) {
            continue;
        }
        if (grpc_lite_metadata_key_is_binary(entry->key)) {
            add_binary_metadata_values_to_map(metadata, entry->key, entry->value);
        } else {
            add_metadata_value_to_map(metadata, entry->key, zend_string_copy(entry->value));
        }
    }
}

void grpc_protocol_add_metadata_map_to_return(zval *return_value, const char *name, grpc_call *call, bool trailing)
{
    zval metadata;

    grpc_protocol_copy_metadata_map(&metadata, call, trailing);
    add_assoc_zval(return_value, name, &metadata);
}
