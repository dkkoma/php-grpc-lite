# grpc_call exchange state

`src/grpc_exchange_state.h` の `grpc_call` は、1 RPC over 1 HTTP/2 stream の交換状態を表す内部structである。

このstructはhot pathで頻繁に触られるため、field分割やsub-struct化は可読性だけでなくcache locality、分岐、callback pathに影響し得る。現時点では実装を分割せず、責務とlifetimeのmapを明文化する。

## Responsibility map

| Responsibility | Fields | Lifetime / owner | Hotness |
|---|---|---|---|
| bench専用の観測 | `fd`, `bytes_sent`, `bytes_received`, `data_read_calls`, `data_recv_calls`, `last_session_error`, frame counter群, `bench` | `PHP_GRPC_LITE_ENABLE_BENCH` buildだけ。production semanticsのownerではない | diagnostic only |
| HTTP/2 connectionとの紐づき | `connection`, `next_active_stream`, `stream_id`, `stream_registered`, `connection_owned` | active HTTP/2 streamとしてconnectionに登録されている間。unaryはstack-owned `grpc_call`、server streamingはresource-owned `grpc_call` | hot |
| stream lifecycle / reset状態 | `stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen` | nghttp2 callbackとread loopが更新し、status resolutionが読む | hot |
| gRPC statusとvalidation flag | `grpc_status`, `grpc_message`, `http_status`, `compressed_response_seen`, `response_message_too_large`, `malformed_response_frame`, `metadata_too_large`, `content_type_seen`, `invalid_content_type`, `unsupported_response_encoding`, `response_queue_limit_exceeded`, `discard_response_body`, `invalid_grpc_status`, `grpc_status_seen`, `initial_grpc_status_seen`, `initial_headers_end_stream` | response header/data processingが更新し、`grpc_lite_status_code_from_call()` とresult buildingが読む | hot |
| response header値 | `content_type`, `grpc_encoding` | response metadata callbackでsetし、cleanupでreleaseする | medium |
| message / metadata limit | `response_message_count`, `max_response_messages`, `max_receive_message_bytes`, `metadata_entry_count`, `metadata_bytes`, `max_response_metadata_bytes` | call setupでlimitをsetし、response processingがincrement/checkする | hot |
| deadline / I/O failure | `timed_out`, `last_io_errno`, `last_ssl_error`, `last_io_error_detail`, `deadline_abs_us` | call setupとsocket/TLS/read/write pathが更新し、status detailが読む | hot |
| response delivery mode | `decode_response_incrementally`, `direct_response_payload`, `queue_response_payloads` | unary / server streaming setupが決める。response parserがbranchする | hot |
| server streaming queue | `response_queue_head`, `response_queue_tail`, `response_queue_count`, `response_queue_bytes` | server streaming resourceがowner。parserがenqueueし、`responses()` pull pathがdequeueする | streaming hot path |
| response metadata list | `metadata_head`, `metadata_tail` | header callbackがappendし、result conversionがcopyする | medium |
| incremental response parser | `response_parse_offset`, `response_header_buf`, `response_header_len`, `response_payload_len`, `response_payload_offset`, `response_current_compressed`, `response_payload` | DATA chunk parserが更新する。malformed/size/compression checkもここに依存する | hot |
| unary body accumulator | `body` | unary direct body accumulation。cleanupでfreeする | unary hot path |
| request writer | `grpc_header`, `grpc_header_len`, `request`, `request_len`, `request_offset`, `pending_data_len`, `pending_write_iov`, `pending_write_iovcnt`, `pending_write_remaining`, `pending_write_payload_len` | nghttp2 data source callbackとsend pathが使う | hot |
| method identity | `method_path` | setupでsetし、trace/debug/status contextで読む | cold to medium |

## Ownership model by call kind

| Call kind | `grpc_call` storage | Request bytes | Response delivery |
|---|---|---|---|
| unary | `grpc_lite_unary_call_perform_core_on_connection()` のstack上 | call実行中はcaller-owned bytesを `request` / `request_len` から参照する | `body` に蓄積し、`grpc_lite_unary_result` へcopyする |
| server streaming | `server_streaming_call_state` resource内に埋め込む | zend stringは `server_streaming_call_state->request` がowner。`grpc_call.request` はそこを指す | payloadは `response_queue_*` にqueueし、wrapper adapterのpull pathが取り出す |

## Why not split immediately

`grpc_call` はsub-struct化したくなる対象だが、直接分割すると性能影響が出やすい。

- nghttp2 callbackは、inbound frameまたはDATA chunkごとにstream lifecycle、validation flag、parser state、metadata、queue stateへ触る。
- request write callbackは、outbound DATA生成時にrequest writer fieldへ触る。
- status resolutionは、多数のflagを優先順位順に読む。
- server streamingは、backpressureとresponse deliveryのためにqueue counterを使う。

そのため、現時点の判断は次の通り。

- 当面はsingle structを維持する。
- この文書をfield ownership mapとして使う。
- field分割は、専用のbefore/after benchmarkとdomain model reviewを通した後だけ行う。

## Candidate future split

将来field groupingの妥当性が実測で示せた場合は、都合ではなくdomain objectごとに分ける。

```text
grpc_call
  grpc_stream_lifecycle
  grpc_status_observation
  grpc_response_metadata_store
  grpc_response_parser
  grpc_request_writer
  grpc_deadline_io_state
  grpc_streaming_delivery_queue
  grpc_bench_call             # bench build専用
```

実装する場合は、少なくとも次を維持する。

- unary / server streamingのstatus/detail挙動。
- metadata shapeとduplicate value preservation。
- read-ahead limitとslow-consumer挙動。
- request frame generation。
- RST_STREAM / GOAWAY / EOF lifecycle;
- production / bench boundary。

## Required verification for a future split

- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- PHPUnit integration
- 代表的なbefore/after benchmark:
  - `spanner-shape`
  - `metadata-header`
  - unary 100B / 100KiB
  - server streaming small message
  - parserまたはqueue fieldを動かす場合はlarge streaming
- HTTP/2/gRPC domain model review
