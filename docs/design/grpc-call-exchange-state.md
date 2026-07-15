# grpc_call exchange state

`src/grpc_exchange_state.h` の `grpc_call` は、1 RPC over 1 HTTP/2 stream の交換状態を表す内部structである。

このstructはhot pathで頻繁に触られるため、field分割やsub-struct化は可読性だけでなくcache locality、分岐、callback pathに影響し得る。現時点では実装を分割せず、責務とlifetimeのmapを明文化する。

## Responsibility map

| Responsibility | Fields | Lifetime / owner | Hotness |
|---|---|---|---|
| bench専用の観測 | `fd`, `bytes_sent`, `bytes_received`, `data_read_calls`, `data_recv_calls`, `last_session_error`, frame counter群, `bench` | `PHP_GRPC_LITE_ENABLE_BENCH` buildだけ。production semanticsのownerではない | diagnostic only |
| HTTP/2 connectionとの紐づき | `connection`, `next_active_stream`, `stream_id`, `stream_registered`, `connection_owned` | active HTTP/2 streamとしてconnectionに登録されている間。unaryはstack-owned `grpc_call`、server streamingはresource-owned `grpc_call` | hot |
| transparent retry attempt | `retry_attempt` | wrapper adapterがattemptごとにsetする。`status_core.c` がattempt outcomeへ写し、wrapper adapterが1回限りの再送判断に使う | hot on failure |
| stream lifecycle / reset状態 | `stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen` | nghttp2 callbackとread loopが更新し、status resolutionが読む | hot |
| response header-block semantic phase | `response_header_phase`, `response_header_block_end_stream`, `response_header_block_protocol_valid`, `response_header_block_incomplete` | `response_header_phase.c` のpure helperがbegin / `:status` / 最初のterminal status field / end / call resetの遷移を所有し、production / diagnostic callbackが共有する。END_STREAM validityとblock-local Trailers-Only candidateをsemantic field commit / metadata ownership gateに使う。`response_header_block_incomplete` はcall-local terminal failureがEND_HEADERS未完了block内で確定したことを共有`grpc_protocol_apply_response_header_terminal_action()`がsetし、productionはconnection terminal quarantine、raw diagnosticの`bench_finish_response_header_terminal_action()`はone-shot fdのnonblocking化に使う。production callのzero-initializationとdiagnostic iterationごとの明示代入でfalseへresetする | header hot path |
| gRPC statusとvalidation flag | `grpc_status`, `grpc_message`, `http_status`, `compressed_response_seen`, `response_message_too_large`, `malformed_response_frame`, `response_header_protocol_error`, `metadata_too_large`, `content_type_seen`, `invalid_content_type`, `unsupported_response_encoding`, `response_queue_limit_exceeded`, `discard_response_body`, `invalid_grpc_status`, `grpc_status_seen`, `initial_grpc_status_seen`, `initial_headers_end_stream`, `trailing_headers_seen` | response header/data processingが更新し、`grpc_lite_status_code_from_call()` とresult buildingが読む。`response_header_protocol_error` はnghttp2のHTTP messaging rejectionやfinal response前DATAを `INTERNAL` にする | hot |
| response header値 | `content_type`, `grpc_encoding` | response metadata callbackでsetし、cleanupでreleaseする | medium |
| message / metadata limit | `response_message_count`, `max_response_messages`, `max_receive_message_bytes`, `metadata_entry_count`, `metadata_bytes`, `wire_response_header_entry_count`, `wire_response_header_bytes`, `max_response_metadata_bytes` | metadata map用counterと、pseudo-header / informational fieldを含むwire work budgetを分けてcall-localに累積する | hot |
| deadline / I/O failure | `timed_out`, `last_io_errno`, `last_ssl_error`, `last_io_error_detail`, `deadline_abs_us` | call setupとsocket/TLS/read/write pathが更新し、status detailが読む | hot |
| response delivery mode | `decode_response_incrementally`, `direct_response_payload`, `queue_response_payloads` | unary / server streaming setupが決める。response parserがbranchする | hot |
| server streaming queue | `response_queue_head`, `response_queue_tail`, `response_queue_count`, `response_queue_bytes` | server streaming resourceがowner。parserがenqueueし、`responses()` pull pathがdequeueする | streaming hot path |
| response metadata list | `metadata_head`, `metadata_tail` | header callbackがappendし、result conversionがcopyする | medium |
| incremental response parser | `response_parse_offset`, `response_header_buf`, `response_header_len`, `response_payload_len`, `response_payload_offset`, `response_current_compressed`, `response_payload` | DATA chunk parserが更新する。malformed/size/compression checkもここに依存する | hot |
| unary body accumulator | `body` | unary direct body accumulation。cleanupでfreeする | unary hot path |
| request writer | `grpc_header`, `grpc_header_len`, `request`, `request_len`, `request_offset`, `pending_data_len`, `pending_write_iov`, `pending_write_iovcnt`, `pending_write_remaining`, `pending_write_payload_len` | nghttp2 data source callbackとsend pathが使う | hot |
| method identity | `method_path` | setupでsetし、trace/debug/status contextで読む | cold to medium |

## Response header-block phase

response header-blockの意味はrawなnghttp2 categoryではなく、`grpc_call.response_header_phase.block_phase` で決める。nghttp2は最初のresponse HEADERSだけを `NGHTTP2_HCAT_RESPONSE` とし、最初のblockが1xxの場合は後続のinformational blockとfinal response HEADERSをいずれも `NGHTTP2_HCAT_HEADERS` として通知する。遷移はnghttp2 / Zendに依存しない `response_header_phase.c` が所有し、productionとbench diagnosticの両方が同じhelperを呼ぶ。

phase transitionは次の通り。

```text
NONE
  -> begin HEADERS, final未観測 -> AWAITING_STATUS
       -> :status 100-199  -> INFORMATIONAL -> final未観測
       -> :status non-1xx -> FINAL_INITIAL -> final観測済み
  -> begin HEADERS, final観測済み -> TRAILING
```

`on_begin_headers_callback()` はhelperでblock開始phaseを設定し、同時にEND_STREAMをblock-local validityとして記録する。`:status` はblock先頭で届くため、残りのfieldをsemantic stateへ反映する前にphaseを確定できる。`INFORMATIONAL` phaseではmetadata list、`http_status`、content-type validation、gRPC status/message/encodingを更新しないが、全fieldをwire header entry/byte budgetへは計上する。`grpc_response_header_phase_allows_status_fields()` はFINAL_INITIAL / TRAILINGかつEND_STREAM付きの場合だけstatus fieldのcommitを認める。`grpc_response_header_phase_on_trailers_only_status_field()` はEND_STREAM付きFINAL_INITIALで `grpc-status` / `grpc-message` / `grpc-status-details-bin` の最初の1 fieldを観測した時にblock-local Trailers-Only candidateを立てる。先行metadataをtrailingへ移し、`grpc_response_header_phase_metadata_is_trailing()` が後続fieldも同じtrailing ownershipへ揃えるため、field kind・parse成否・順序でblockが分裂しない。

header-block phaseはfield callback時点の処理境界を所有する。final response後のHEADERSがEND_STREAMを持たない場合はblock内のstatus/metadataをquarantineし、nghttp2の `on_invalid_frame_recv_callback()` 観測を `response_header_protocol_error` へ反映する。END_STREAMなし `FINAL_INITIAL` でstatus fieldを観測した場合は、production / diagnostic共有helperがfield callback時点で `invalid_grpc_status` を確定し、対象streamへ `RST_STREAM(CANCEL)` をsubmitする。informational HEADERSのEND_STREAM、END_STREAMなしtrailing HEADERS、wire-header budget超過を含め、call-local terminal failureがEND_HEADERS未完了block内で確定した場合は、failure taxonomyやRST codeとは独立した`grpc_protocol_apply_response_header_terminal_action()`が `response_header_block_incomplete` をsetする。productionはconnection-localなclose-after-pending-flush stateで新規stream admissionを止め、pending control frameの有限flush後にconnectionをdeadへ移す。raw diagnosticの`bench_finish_response_header_terminal_action()`は同じclassificationを読み、one-shot fdをnonblockingへ切り替えて有限終了させる。これにより対象callを各primary taxonomyのままsilent peerから切り離し、既存sibling callはsession / socketを再駆動せず `UNAVAILABLE`、follow-up RPCはfresh connectionを使う。このterminal quarantineはadmit済みstreamの完走を許すGOAWAY drainingとは別のlifecycleである。nghttp2がfinal response前DATAをreceived-frame callbackへ渡さず拒否する経路は、library-generated outbound `RST_STREAM(PROTOCOL_ERROR)` をframe-send callbackで観測して同じtaxonomyへ写像する。valid HEADERS完了時はhelperの `end()` で完了roleを維持したままcurrent phaseを `NONE` へ戻す。遷移表、status commit / metadata roleのtruth table、call resetは `tests/unit/test_response_header_phase.c` で固定する。

## Ownership model by call kind

| Call kind | `grpc_call` storage | Request bytes | Response delivery |
|---|---|---|---|
| unary | `grpc_lite_unary_call_perform_core_on_connection()` のstack上 | call実行中はcaller-owned bytesを `request` / `request_len` から参照する | `body` に蓄積し、`grpc_lite_unary_result` へcopyする |
| server streaming | `server_streaming_call_state` resource内に埋め込む | zend stringは `server_streaming_call_state->request` がowner。`grpc_call.request` はそこを指す | payloadは `response_queue_*` にqueueし、wrapper adapterのpull pathが取り出す |

## Connection / stream ownership

`grpc_call` はHTTP/2 streamとしてconnectionへ登録される間、connection lifetimeのownerも1つ持つ。ここで重要なのは、callback lookup用のactive登録と、connection破棄を遅延するowner countを同じ意味にしないことである。

`register_grpc_call_stream(connection, call)` は、stream id確定後の所有権確立点である。成功時はnghttp2 stream user dataに `call` を登録し、`call->connection`、`stream_registered`、`connection_owned` を設定し、connectionのactive stream list、`active_stream_count`、`stream_owner_count` を更新する。

`unregister_grpc_call_stream(call)` はactive登録だけを外す。nghttp2 stream user dataとactive stream listから `call` を外し、`stream_registered` と `active_stream_count` を更新するが、`stream_owner_count` は減らさない。stream close callback、GOAWAY、RST_STREAMでactive登録が外れた後でも、status resolution、result construction、resource cleanupがconnection情報を読むことがあるためである。

owner countを減らすのはowner clear系の責務である。unaryの `grpc_call` はstack上にあり、`clear_connection_call_owner()` は `connection_owned` を落として `stream_owner_count` を減らすが、`call->connection` は `NULL` にしない。server streamingの `grpc_call` はPHP resourceに埋め込まれるため、`clear_connection_server_streaming_call_state_owner()` はownerを解放した後に `state->call.connection` を `NULL` にする。

persistent connection cache entryは、connectionの唯一のownerではない。cacheから外したconnectionでも `stream_owner_count > 0` なら即時破棄せず、`detached_from_cache` として最後のowner clearまで残す。最後に `stream_owner_count == 0` になった時点で `destroy_detached_connection_if_unowned()` が破棄できる。

この領域のinvariantは次の通り。

- `stream_registered == true` なら、nghttp2 stream user dataとactive stream listに登録されている。
- `active_stream_count` はcallback lookup対象のstream数であり、connection lifetime owner数ではない。
- `connection_owned == true` なら、その `grpc_call` は `stream_owner_count` を1つ持つ。
- owner clearは `connection_owned == true` のときだけ `stream_owner_count` を減らす。
- detached connectionは `stream_owner_count == 0` になるまで破棄しない。
- unaryの `call->connection` はowner clear後もstack lifetime内で残り得る。
- server streamingの `state->call.connection` はowner clear時に `NULL` にする。

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
