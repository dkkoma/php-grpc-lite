# grpc_call exchange state

`src/grpc_exchange_state.h` の `grpc_call` は、1 RPC over 1 HTTP/2 stream の交換状態を表す内部structである。

このstructはhot pathで頻繁に触られるため、field分割やsub-struct化は可読性だけでなくcache locality、分岐、callback pathに影響し得る。現時点では実装を分割せず、責務とlifetimeのmapを明文化する。

## Responsibility map

| Responsibility | Fields | Lifetime / owner | Hotness |
|---|---|---|---|
| bench専用の観測 | `fd`, `bytes_sent`, `bytes_received`, `data_read_calls`, `data_recv_calls`, `last_session_error`, frame counter群, `bench` | `PHP_GRPC_LITE_ENABLE_BENCH` buildだけ。`bench.connection_header_block_incomplete` は1 sessionを跨ぐconnection-terminal markerでiteration reset対象外。production semanticsのownerではない | diagnostic only |
| HTTP/2 connectionとの紐づき | `connection`, `next_active_stream`, `stream_id`, `stream_registered`, `connection_owned` | active HTTP/2 streamとしてconnectionに登録されている間。unaryはstack-owned `grpc_call`、server streamingはresource-owned `grpc_call` | hot |
| transparent retry attempt | `retry_attempt` | wrapper adapterがattemptごとにsetする。`status_core.c` がattempt outcomeへ写し、wrapper adapterが1回限りの再送判断に使う | hot on failure |
| stream lifecycle / reset状態 | `stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen` | nghttp2 callbackとread loopが更新し、status resolutionが読む | hot |
| response header-block semantic phase | `response_header_phase`, `response_header_block_end_stream`, `response_header_block_protocol_valid`, `response_header_block_incomplete` | `response_header_phase.c` のpure helperがbegin / `:status` / 最初のterminal status field / end / call resetの遷移を所有し、production / diagnostic callbackが共有する。END_STREAM validityとblock-local Trailers-Only candidateをsemantic field commit / metadata ownership gateに使う。`grpc_response_header_phase_requires_connection_terminal_on_abandonment()` は `block_phase != NONE` をlive callがinbound blockを所有中であることへ写像する。`response_header_block_incomplete` はterminal field classificationまたは `grpc_protocol_mark_abandoned_response_header_block()` がshared terminal actionへ到達した結果を表す。client-owned RST producerはmark後にselected RSTをsubmitし、nghttp2-owned rejectionはmarkだけを呼ぶ。productionはconnection terminal quarantine、raw diagnosticの `bench_finish_abandoned_response_header_block()` はpoll-loop failure時にsession-terminal marker、deadline時のbest-effort CANCEL、one-shot fdの有限終了を所有する。production callのzero-initializationとdiagnostic iterationごとの明示代入でcall-local markerをfalseへresetする | header hot path |
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

### Field notification routing

nghttp2から届くresponse fieldは、個別のheader nameをcallbackごとに特例処理せず、`grpc_response_header_field_class` とcurrent phaseから `grpc_response_header_field_route` を決める。application callbackへ公開されたfieldはnormal / invalidを問わずroute判定より先にshared wire budgetへ計上する。nghttp2がstrict HTTP messaging rejectionしたfieldはapplicationへname / valueが公開されないため、そのfield自体のentry / byteは計上できないが、`REJECTED` eventとして同じterminal routeへ入る。missing `:status` やnon-terminal trailersのようなblock-end HTTP messaging rejectionも同じeventへ写像し、それまで公開されたfieldのbudgetを維持する。

| nghttp2 notification / field class | Budget visibility | `NONE` | `AWAITING_STATUS` | `INFORMATIONAL` | `FINAL_INITIAL` | `TRAILING` | Terminal action / RST owner |
|---|---|---|---|---|---|---|---|
| `on_header_callback()` / `STATUS` (`:status`) | route前に1 field計上 | protocol error | phaseを1xx / finalへ遷移 | protocol error | protocol error | protocol error | complete blockはnghttp2のframe-end rejection、END_HEADERS未完了はclient-selected `PROTOCOL_ERROR`。通常、duplicate / misplaced `:status` はfield callback前にstrict rejectされるが、pure routeもfail closedする |
| `on_header_callback()` / `REGULAR` | route前に1 field計上 | protocol error | regular-before-`:status` protocol error | semantic stateへ反映せずignore | initial metadata / content-type / status-field gateへprocess | trailing metadata / status-field gateへprocess | terminal classificationがEND_HEADERS未完了ならclientが選んだRSTをsubmitする |
| `on_invalid_header_callback()` / `INVALID_REGULAR`（empty nameを含む） | route前に1 field計上 | protocol error | regular-before-`:status` protocol error | semantic ignore | semantic ignore | semantic ignore | invalid fieldをmetadataへ昇格しない。terminal classificationがEND_HEADERS未完了ならclient-selected `PROTOCOL_ERROR` |
| strict field rejectionまたはblock-end HTTP messaging rejection / `REJECTED` | strict fieldはcallbackなしで未計上。block-endまでに公開済みのfieldは計上済み | protocol error | protocol error | protocol error | protocol error | protocol error | `on_invalid_frame_recv_callback()` がshared terminal classificationを呼ぶ。RSTはnghttp2 ownershipなので重複submitせず、END_HEADERS未完了のconnection lifecycleだけをmarkする |
| local `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` 後のinvalid-frame observation | producer callbackで計上済み。二重計上しない | producerのprimary taxonomyを維持 | 同左 | 同左 | 同左 | 同左 | budget超過の `RESOURCE_EXHAUSTED` / selected `CANCEL`、field-route rejectionの `INTERNAL` / selected `PROTOCOL_ERROR` などproducer-owned classificationを維持し、shared incomplete-block lifecycleだけをidempotentに再適用する |
| 未知のfield class | applicationへ公開済みならroute前に計上。非公開なら計上不可 | protocol error | protocol error | protocol error | protocol error | protocol error | enum追加やnghttp2通知経路追加が明示routeを持たなくてもdefaultはterminal protocol errorとし、ignore / successへ落とさない |

normal / invalid field callbackが返す値は、処理継続の`0`、stream-local stopの`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`、allocationまたはcallback fatalの`NGHTTP2_ERR_CALLBACK_FAILURE`である。`NGHTTP2_ERR_PAUSE`はresponse header pathでは使わない。`TEMPORAL`はnghttp2のinvalid-frame observerへ再到達するためshared terminal lifecycleをone-shot前提にせずidempotentにする。`CALLBACK_FAILURE`はfield semantic rejectionではなくouter transportがconnection failureとして所有する。

既にclose / unregisterされたstream、またはforeign streamのHEADERSはlive `grpc_call`を持たず、上のfield notification routing tableへ入らない。これはfield classや`NONE` phaseではなくconnection/session lifecycleである。production / diagnosticの`on_begin_frame`はshared pure predicateでEND_HEADERS未完了のunowned HEADERSだけを検知し、call taxonomyやstream RSTを変更せずconnection-terminal markerを設定する。active streamのfragmented HEADERSは従来どおりcall-local routeが所有する。

active streamの所有はblock完了まで無条件に続くわけではない。`on_begin_headers_callback()` から `grpc_response_header_phase_end()` までの `block_phase != NONE` は、call-local semantic phaseであると同時に「このlive callがconnection-globalなinbound header blockを所有している」ことを表す。deadline、明示cancel、server streaming resource destructor、またはstream-local semantic-error teardownがphase endより先にcallを放棄するときは、field notificationではないため上表へsynthetic routeを追加しない。これらのlive local abandonment pathが収束する `cancel_grpc_call_stream()` は `grpc_response_header_phase_requires_connection_terminal_on_abandonment()` を確認し、call pointerとphaseを失う前に `grpc_protocol_mark_abandoned_response_header_block()` から既存のincomplete-header terminal actionへhandoffする。

このhandoffはprimary failure classificationを所有しない。deadlineは `DEADLINE_EXCEEDED`、明示cancelは `CANCELLED`、stream-local semantic failureは既に選択済みのtaxonomyを維持し、targetのcaller-selected RST codeも変更しない。transport actionだけが新規admissionを止め、pending RST/control frameへ有限なflush機会を与えた後にconnectionをdeadへ移す。fatal I/O / nghttp2 error pathはabandonment predicateに依存せず先にconnectionをdead化するため、その後のunregister / owner clearはlocal bookkeepingだけを行う。productionはpersistent `h2_connection` のclose-after-pending-flush stateをconsumerとする。raw diagnosticはpoll-loop failure直後に `bench_finish_abandoned_response_header_block()` を呼び、同じopen-block predicateからsession-terminal markerへ写像する。deadline abandonmentではtarget `RST_STREAM(CANCEL)`をsubmitし、既にnonblockingとなったone-shot fdへbest-effort flushして終了する。どちらもcall-local markerのiteration resetとconnection/session markerのlifetimeを混同しない。

`on_begin_headers_callback()` はhelperでblock開始phaseを設定し、同時にEND_STREAMをblock-local validityとして記録する。valid responseでは`:status`がregular fieldより前に届くため、残りのfieldをsemantic stateへ反映する前にphaseを確定できる。`grpc_response_header_classify_reported_field()` と `grpc_response_header_route_field()` はnormal / invalid callbackを同じclosed tableへ写像し、`AWAITING_STATUS` のregular field、misplaced pseudo-header、未知classをfail closedする。`INFORMATIONAL` phaseではmetadata list、`http_status`、content-type validation、gRPC status/message/encodingを更新しないが、application callbackへ公開された全fieldをwire header entry/byte budgetへ計上する。`grpc_response_header_phase_allows_status_fields()` はFINAL_INITIAL / TRAILINGかつEND_STREAM付きの場合だけstatus fieldのcommitを認める。`grpc_response_header_phase_on_trailers_only_status_field()` はEND_STREAM付きFINAL_INITIALで `grpc-status` / `grpc-message` / `grpc-status-details-bin` の最初の1 fieldを観測した時にblock-local Trailers-Only candidateを立てる。先行metadataをtrailingへ移し、`grpc_response_header_phase_metadata_is_trailing()` が後続fieldも同じtrailing ownershipへ揃えるため、field kind・parse成否・順序でblockが分裂しない。

header-block phaseはfield callback時点の処理境界を所有する。final response後のHEADERSがEND_STREAMを持たない場合はblock内のstatus/metadataをquarantineし、nghttp2の `on_invalid_frame_recv_callback()` 観測を `response_header_protocol_error` へ反映する。END_STREAMなし `FINAL_INITIAL` でstatus fieldを観測した場合は、production / diagnostic共有helperがfield callback時点で `invalid_grpc_status` を確定し、対象streamへ `RST_STREAM(CANCEL)` をsubmitする。regular fieldが`:status`より先行したblockは `RST_STREAM(PROTOCOL_ERROR)`、informational HEADERSのEND_STREAM、END_STREAMなしtrailing HEADERS、wire-header budget超過などは各caller固有のRST codeを選ぶ。call-local terminal failureがEND_HEADERS未完了block内で確定した場合は、failure taxonomyやRST ownershipとは独立した`grpc_protocol_mark_response_header_terminal_action()`が `response_header_block_incomplete` をsetする。clientがRSTを所有するproducerは`grpc_protocol_apply_response_header_terminal_action()`がmarkとsubmitを行い、strict rejectionではnghttp2-owned RSTを重複submitせずmarkだけを行う。productionはconnection-localなclose-after-pending-flush stateで新規stream admissionを止め、pending control frameの有限flush後にconnectionをdeadへ移す。raw diagnosticのterminal-action consumerは同じclassificationを読み、one-shot fdをnonblockingへ切り替えて有限終了させる。production / diagnostic callbackはいずれも同じfield class / phase predicateを使い、diagnostic固有の観測値をproduction semanticsのownerにしない。これにより対象callを各primary taxonomyのままsilent peerから切り離し、既存sibling callはsession / socketを再駆動せず `UNAVAILABLE`、follow-up RPCはfresh connectionを使う。このterminal quarantineはadmit済みstreamの完走を許すGOAWAY drainingとは別のlifecycleである。nghttp2がfinal response前DATAをreceived-frame callbackへ渡さず拒否する経路は、library-generated outbound `RST_STREAM(PROTOCOL_ERROR)` をframe-send callbackで観測して同じtaxonomyへ写像する。valid HEADERS完了時はhelperの `end()` で完了roleを維持したままcurrent phaseを `NONE` へ戻す。phase遷移、field class × phase route、status commit / metadata role、unknown-class default、call resetは `tests/unit/test_response_header_phase.c` のtableで固定する。

## Ownership model by call kind

| Call kind | `grpc_call` storage | Request bytes | Response delivery |
|---|---|---|---|
| unary | `grpc_lite_unary_call_perform_core_on_connection()` のstack上 | call実行中はcaller-owned bytesを `request` / `request_len` から参照する | `body` に蓄積し、`grpc_lite_unary_result` へcopyする |
| server streaming | `server_streaming_call_state` resource内に埋め込む | zend stringは `server_streaming_call_state->request` がowner。`grpc_call.request` はそこを指す | payloadは `response_queue_*` にqueueし、wrapper adapterのpull pathが取り出す |

## Connection / stream ownership

`grpc_call` はHTTP/2 streamとしてconnectionへ登録される間、connection lifetimeのownerも1つ持つ。ここで重要なのは、callback lookup用のactive登録と、connection破棄を遅延するowner countを同じ意味にしないことである。

`register_grpc_call_stream(connection, call)` は、stream id確定後の所有権確立点である。成功時はnghttp2 stream user dataに `call` を登録し、`call->connection`、`stream_registered`、`connection_owned` を設定し、connectionのactive stream list、`active_stream_count`、`stream_owner_count` を更新する。

`unregister_grpc_call_stream(call)` はactive登録だけを外す。nghttp2 stream user dataとactive stream listから `call` を外し、`stream_registered` と `active_stream_count` を更新するが、`stream_owner_count` は減らさない。stream close callback、GOAWAY、RST_STREAMでactive登録が外れた後でも、status resolution、result construction、resource cleanupがconnection情報を読むことがあるためである。connection-terminal判断はここで再実装しない。live local abandonmentは先行する `cancel_grpc_call_stream()` がopen blockをhandoffし、fatal error pathは先にconnectionをdead化するため、unregisterはcall pointerを外すbookkeepingに留める。

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
- `block_phase != NONE` のlive callをlocal abandonmentで閉じるときは、`cancel_grpc_call_stream()` がRST submit前にincomplete inbound header blockをconnection lifecycleへhandoffする。
- fatal I/O / nghttp2 error後のunregister / owner clearはdead connection上のlocal bookkeepingだけを行う。
- abandonment handoffはcall statusとRST codeを上書きせず、connection reuse可否だけをterminalへ変える。

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
