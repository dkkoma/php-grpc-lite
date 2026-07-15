# protocol classification boundary

この文書は、response processingで起きるgRPC protocol failureの分類と、HTTP/2 transport actionの責務境界を読むための地図である。

現行実装では、nghttp2 callback、gRPC frame parser、metadata storage、status resolution、`RST_STREAM` 送信が `src/transport.c` にまとまっている。hot pathを無理に分割すると性能や挙動に影響するため、この文書ではまず「どの責務がどこにあるか」と「将来分割するときの境界」を明文化する。runtime挙動は変えない。

## Layers

| Layer | Owns | Does not own |
|---|---|---|
| Pure protocol helper | byte列をgRPC仕様上の値へ分類する | `grpc_call` 更新、nghttp2操作、PHP result作成 |
| Call classification | 1 RPC / 1 streamで何が起きたかを `grpc_call` に記録する | connection cache破棄、retry orchestration |
| Status taxonomy | `grpc_call` の分類結果をgRPC status codeとattempt outcomeへ畳む | frame parsing、socket/TLS I/O、retry実行 |
| Transport action | stream cancel、connection dead/draining、persistent cache detachを実行する | gRPC status codeの意味づけ |
| PHP result bridge | metadata/status/detailsをPHP-visible shapeへ変換する | transport lifecycle判断 |

現在の主な対応は次の通り。

- Pure protocol helper: `src/protocol_core.c`
- Call classification: `src/transport.c` の nghttp2 callback / response parser
- Status taxonomy: `src/status_core.c`
- Transport action: `src/transport.c` の connection lifecycle / `RST_STREAM` / cache detach helper
- PHP result bridge: `resolve_grpc_call_status()`, `add_status_result_to_return()`, wrapper result construction

## Classification State

`grpc_call` はcombined stateであり、HTTP/2 stream identity、parser state、metadata/status、request writer、diagnostic stateを同じstructに持つ。classificationとして読むfieldは次のgroupに分かれる。

| Classification | Fields | Set by | Status mapping |
|---|---|---|---|
| Response header-block phase / field route | `response_header_phase`, `response_header_block_end_stream`, `response_header_block_protocol_valid`, `response_header_block_incomplete` | shared phase / field-class helper、header callbacks、invalid-frame callback | statusへ直接写像しない。informational / final initial / trailingのfield反映境界、unknown-class fail-closed、block-local commit / incomplete lifecycle gateを決める |
| Unowned incomplete HEADERS | call-local fieldなし。connectionの`close_after_pending_flush`、diagnostic sessionのsticky terminal marker | shared connection-scope frame-start predicate、production / diagnostic `on_begin_frame` | 完了済みcallのstatusへ写像しない。closed / foreign streamの未完了HPACK blockとしてconnectionをterminal化する |
| Abandoned open response HEADERS | abandonment直前の `response_header_phase.block_phase != NONE`、`response_header_block_incomplete`、connection / diagnostic sessionのterminal marker | `grpc_response_header_phase_requires_connection_terminal_on_abandonment()`、production `cancel_grpc_call_stream()`、raw diagnostic `bench_finish_abandoned_response_header_block()` | deadline / explicit cancel / stream-local semantic errorのstatusへ直接写像しない。call-local owner消失前に `grpc_protocol_mark_abandoned_response_header_block()` からincomplete HPACK lifecycleだけをconnectionへhandoffする。fatal I/O / nghttp2 errorはpredicateと独立にconnection deadを所有する |
| Response header protocol error | `response_header_protocol_error` | `grpc_protocol_handle_response_header_field_route()`、invalid-frame callback、final前DATAに対するoutbound protocol-RST observer、stream close fallback | `INTERNAL`。`grpc-status` / HTTP status fallbackより優先 |
| Deadline exceeded | `timed_out` | send/recv deadline path | `DEADLINE_EXCEEDED` |
| Invalid gRPC status field | `invalid_grpc_status`, `grpc_status_seen`, `initial_grpc_status_seen`, `initial_headers_end_stream` | `on_header_callback()`, shared response-status-field observer | `UNKNOWN` |
| HTTP status fallback | `http_status`, `grpc_status` absent | `on_header_callback()` | HTTP status mapping in `status_core.c` |
| Invalid content type | `content_type_seen`, `content_type`, `invalid_content_type` | `on_header_callback()`, response HEADERS finalization | `UNKNOWN` |
| Metadata too large | `metadata_too_large`, semantic metadata counters, `wire_response_header_*` | header field accounting、`grpc_protocol_add_response_metadata_entry()` | `RESOURCE_EXHAUSTED`。final HTTP status観測前でも優先 |
| Message too large | `response_message_too_large`, parser counters | response DATA parser | `RESOURCE_EXHAUSTED` |
| Read-ahead queue too large | `response_queue_limit_exceeded`, queue counters | server streaming queue helper | `RESOURCE_EXHAUSTED` |
| Malformed gRPC frame | `malformed_response_frame` | response DATA parser, invalid PUSH_PROMISE | `INTERNAL` |
| Unsupported compression | `compressed_response_seen`, `unsupported_response_encoding`, `grpc_encoding` | response DATA parser (Compressed-Flag=1 のみ。`grpc-encoding` header は観測のみで、flag=0 message は未対応 encoding 下でも成功する) | `INTERNAL` |
| Missing trailers | `stream_closed`, `stream_error_code == NGHTTP2_NO_ERROR`, `grpc_status` absent, `http_status == 200`, `!initial_headers_end_stream`, `!trailing_headers_seen` | `on_stream_close_callback()`, `on_frame_recv_callback()` (DATA END_STREAM のみ。HEADERS END_STREAM は `UNKNOWN`、grpc-go `handleData` / `operateHeaders` 準拠) | `INTERNAL` |
| Stream reset | `stream_reset_seen`, `stream_error_code` | `on_frame_recv_callback()` | HTTP/2 error code mapping in `status_core.c` |
| GOAWAY refused stream | `stream_refused_seen`, `stream_error_code` | GOAWAY handling in `on_frame_recv_callback()` | `UNAVAILABLE` |

分類fieldは「RPCとして何が起きたか」を表す。`RST_STREAM` を送るか、connectionをdead/drainingにするか、persistent cacheから外すか、同じRPCをtransparent retryするかは別の判断である。`status_core.c` は `grpc_lite_attempt_outcome` として `transparent_retryable_unprocessed` / refused kind / response started を返すが、retry実行とcache処理はwrapper adapter側のorchestrationが担当する。

## Transport Action State

Transport actionはHTTP/2 connection / stream lifecycleを変える副作用である。

| Action | Owner | Trigger examples | Notes |
|---|---|---|---|
| `RST_STREAM(CANCEL)` | stream-local transport action | message too large、metadata too large、unsupported compression、malformed frame、read-ahead limit、client cancel/destructor | 原則として対象streamだけを閉じ、connectionがusableなら再利用可能 |
| `RST_STREAM(PROTOCOL_ERROR)` | stream-local protocol rejection | invalid `PUSH_PROMISE`、response field routeのprotocol rejection | client-selected rejectionはclientがsubmitする。strict HTTP messaging rejectionはnghttp2がsubmitするため重複させない |
| mark connection dead | connection lifecycle | socket EOF/error、TLS fatal error、nghttp2 session error、stream registration failure、END_HEADERS未完了blockでpending control frameをflushした後 | persistent cacheから外す対象。未完了inbound HPACK blockではRST ownerにかかわらず全ownerにterminalで、対象call以外の既存siblingも追加I/Oなしでconnection failureへ収束する |
| mark connection draining | connection lifecycle | received GOAWAY、preflight drain limit exceeded | 新規streamへ使わない。GOAWAYのlast stream idより大きいactive streamはrefused扱い。ただし `last_stream_id=2147483647` は二段階GOAWAYのdraining通知として既存streamを閉じない |
| detach/remove persistent cache entry | cache lifecycle | connectionがusableでなくなった、またはownerが残るconnectionをcacheから外す | stream ownerが残る場合はdetachedとして遅延破棄 |

stream-local failureはconnection failureではない。message size、metadata size、compression、malformed response frame、invalid content-typeはRPC statusへ変換し、socket/TLS/nghttp2 sessionが壊れていなければpersistent connectionは次RPCで再利用できる。

## Current Mixed Hot Paths

現行コードでは性能と単純さを優先し、分類と一部transport actionが同じhot pathにある。

| Function | Classification responsibility | Transport action in same path |
|---|---|---|
| `on_begin_headers_callback()` | shared helperでphaseを開始し、END_STREAMからblock-local validityを決める | なし |
| `on_begin_frame_callback()` | live callを持たないHEADERS frameのEND_HEADERS completenessをshared pure predicateで判定 | incompleteならcallへ帰属させずconnection close-after-pending-flush actionを設定。active fragmented HEADERSは対象外 |
| `on_header_callback()` / `on_invalid_header_callback()` | applicationへ公開された全fieldのwire budget計上、shared field-class × phase route、`:status`でのphase確定、valid blockのstatus/metadata/content-type分類 | routeがprotocol rejectionなら `RST_STREAM(PROTOCOL_ERROR)`、wire budget超過またはEND_STREAMなしFINAL_INITIAL status fieldは `RST_STREAM(CANCEL)`。terminal classificationがEND_HEADERS未完了なら共通close-after-pending-flush actionを設定 |
| invalid-frame / frame-send callback | field callbackを迂回するstrict field rejection、missing `:status` / non-terminal trailersなどのblock-end HTTP messaging rejection、local TEMPORAL stop、final response前DATAに対するlibrary-generated `RST_STREAM(PROTOCOL_ERROR)` をshared classificationへ接続 | HTTP messaging rejectionのRSTはnghttp2 ownershipとして重複submitしない。TEMPORALでは既存taxonomy / selected RSTを維持し、END_HEADERS未完了のlifecycle markだけをidempotentに適用する |
| `on_frame_recv_callback()` | GOAWAY refusal、RST_STREAM、missing content-type、Trailers-Only misuse、PUSH_PROMISE拒否、phase endの分類 | GOAWAYでdraining化、PUSH_PROMISEで `RST_STREAM(PROTOCOL_ERROR)` |
| `cancel_grpc_call_stream()` / `bench_finish_abandoned_response_header_block()` | phase end前にlive local ownerを放棄するかをshared pure predicateで判定。field-class routeは再実行しない | open blockならtarget taxonomy / selected RSTを維持したまま共通incomplete-header actionへhandoffし、productionはbounded control flush後にconnectionをdead化する。raw diagnosticはpoll-loop failure直後にsession-terminal markerをsetし、timeoutならexact CANCELをnonblockingでbest-effort flushして有限終了する。fatal error後のunregister / owner clearはdead connection上のbookkeeping |
| `grpc_protocol_validate_response_message_lengths()` | unary/body aggregation pathのgRPC frame、message count、compressed flag、message size分類 | stream-local failureで `RST_STREAM(CANCEL)` |
| `grpc_protocol_process_response_data_direct()` | server streaming direct parserのgRPC frame、message size、compressed flag、queue delivery分類 | stream-local failureで `RST_STREAM(CANCEL)` |
| `mark_server_streaming_read_ahead_limit_exceeded()` | read-ahead queue limit分類 | `RST_STREAM(CANCEL)` |

これは現時点で許容する。ただし、将来refactorするときは分類結果を先に作り、transport actionを小さなdispatcherへ寄せる方向にする。

## Boundary Rules

今後この領域を変更するときは、次の境界を守る。

1. Pure helperは副作用を持たない。
   - `protocol_core.c` はbyte列の妥当性、status parse、timeout formatのようなpure logicだけを持つ。
   - `nghttp2_session`、`grpc_call`、Zend allocationへ依存しない。

2. ClassificationはRPC-local stateだけを更新する。
   - `grpc_call` flags、parser offsets、metadata counters、message countersを更新する。
   - connection cache、socket/TLS state、persistent entryを直接判断しない。

3. Transport actionはHTTP/2 lifecycleだけを更新する。
   - `RST_STREAM`、connection dead/draining、cache detach/remove、owner releaseを担当する。
   - gRPC status codeの優先順位を再実装しない。

4. Status taxonomyは分類結果の優先順位だけを持つ。
   - `status_core.c` は `grpc_call` flagsからstatus codeを返す。
   - socket/TLS I/Oやnghttp2送信を行わない。

5. PHP result bridgeは表示形だけを作る。
   - details文字列、metadata map、typed resultを作る。
   - connection再利用可否やstream cancel要否を決めない。

## Failure Matrix

| Scenario | Classification | Transport action | Connection reuse | Verification |
|---|---|---|---|---|
| 1xx informational response | blockを `INFORMATIONAL` としてsemantic fieldを隔離し、wire header budgetには累積。後続final responseを `FINAL_INITIAL` として処理 | valid sequenceでは受動的にfinal responseを待つ | reusable if connection usable | `tests/phpt/022-error-and-http-validation.phpt`, `tests/phpt/042-informational-1xx-adversarial.phpt` |
| live callがfragmented response HEADERS中にabandonされる | `block_phase != NONE` をshared pure predicateでopen blockと分類。deadline / cancel / stream-local semantic errorのprimary taxonomyは維持 | live local abandonmentが収束する `cancel_grpc_call_stream()` がselected target RSTを変えずincomplete-header lifecycleへhandoff。productionは新規admission停止、bounded pending-control flush、dead化。raw diagnostic deadline pathもexact CANCEL、session-terminal marker、nonblocking finite finishへ収束する | terminal。follow-upはfresh connection | `tests/phpt/042-informational-1xx-adversarial.phpt`, `tests/phpt/043-informational-1xx-bench-parity.phpt`, `tests/unit/test_response_header_phase.c` |
| malformed response header sequence | shared field-class × phase routeで `response_header_protocol_error`。normal / recoverably-invalid / strict-rejected / unknown classを同じfail-closed modelへ畳む | client callbackが確定した未完了rejectionはclient-selected `RST_STREAM(PROTOCOL_ERROR)`。strict HTTP rejectionはnghttp2-owned RSTを再submitせず、どちらもincomplete HPACK lifecycle markからflush後にconnectionをdead化 | complete blockはconnection usableならreusable。未完了blockはterminalでfollow-upはfresh | `tests/phpt/042-informational-1xx-adversarial.phpt`, `tests/phpt/043-informational-1xx-bench-parity.phpt`, `tests/unit/test_response_header_phase.c`, `tests/unit/test_status_core.c` |
| status fieldがEND_STREAMなしfinal initial blockにある、または`grpc-status` valueがinvalid | `invalid_grpc_status` | non-terminal final initialはfield callback時に `RST_STREAM(CANCEL)`。END_HEADERS未完了ならCANCEL flush後にconnectionをdead化し、対象は `UNKNOWN`、既存siblingは追加I/Oなしで `UNAVAILABLE`。value parse failureはstatus taxonomyへ反映 | END_HEADERS完了かつconnection usableならreusable。未完了なら全ownerでterminalとなりfollow-upはfresh connection | `tests/phpt/022-error-and-http-validation.phpt`, `tests/phpt/042-informational-1xx-adversarial.phpt`, `tests/phpt/043-informational-1xx-bench-parity.phpt`, `tests/unit/test_protocol_core.c`, `tests/unit/test_response_header_phase.c`, `tests/unit/test_status_core.c` |
| `content-type` missing / invalid | `invalid_content_type` | body discard。DATAが来た場合はstream-local cancel候補 | reusable if connection usable | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| `grpc-encoding` unsupported | `unsupported_response_encoding` | body discard / stream cancel | reusable if connection usable | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php` |
| compressed flag in gRPC frame | `compressed_response_seen` | `RST_STREAM(CANCEL)` | reusable if connection usable | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php` |
| malformed gRPC frame | `malformed_response_frame` | `RST_STREAM(CANCEL)` | reusable if connection usable | `tests/phpt/022-error-and-http-validation.phpt`, `tests/phpt/024-control-semantics.phpt` |
| response message too large | `response_message_too_large` | `RST_STREAM(CANCEL)` | reusable if connection usable | `tests/phpt/025-resource-limits.phpt` |
| response metadata / wire header budget too large | `metadata_too_large`。final HTTP status前でも `RESOURCE_EXHAUSTED` とheader/metadata budget専用details | `RST_STREAM(CANCEL)`。END_HEADERS未完了ならflush後にconnectionをdead化 | complete blockはconnection usableならreusable。未完了blockはterminalでfollow-upはfresh | `tests/phpt/025-resource-limits.phpt`, `tests/phpt/042-informational-1xx-adversarial.phpt`, `tests/phpt/043-informational-1xx-bench-parity.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| server streaming read-ahead too large | `response_queue_limit_exceeded` | `RST_STREAM(CANCEL)` | reusable if connection usable | server streaming resource limit / lifecycle coverage |
| server `RST_STREAM(REFUSED_STREAM)` before response starts | `stream_reset_seen`, `stream_error_code`, attempt outcome | inbound observation only。初回attemptかつresponse未開始ならwrapper adapterが同じconnectionへ1回transparent retry | reusable if connection usable | `tests/phpt/024-control-semantics.phpt`, `tests/unit/test_status_core.c` |
| GOAWAY refused before response starts | connection draining、`stream_refused_seen`, attempt outcome | no new stream on this connection。初回attemptかつresponse未開始ならwrapper adapterがcacheから外して新connectionへ1回transparent retry | not reused for new RPC | `tests/phpt/024-control-semantics.phpt`, `tests/unit/test_status_core.c` |
| GOAWAY MaxInt32 | connection draining | no new stream on this connection。既存streamはrefusedにしない | not reused for new RPC after current stream completes | `tests/phpt/024-control-semantics.phpt` |
| socket/TLS EOF or fatal error | connection error detail | mark connection dead、remove/detach cache | not reused | `tests/phpt/024-control-semantics.phpt`, TLS PHPT |

## Refactor Guidance

このboundaryを実装上も明確にする場合は、性能影響を避けるため段階を分ける。

1. `grpc_call` flagの意味を変えず、classification helper名だけを導入する。
2. `RST_STREAM(CANCEL)` の送信を小さなtransport action helperへ寄せる。
3. DATA parserから `nghttp2_session *` を直接受け取る形を見直す場合は、before/after benchmarkを取る。
4. status code priorityを変えずに `status_core.c` のC unitを先に拡張する。
5. GOAWAY / EOF / RST_STREAM lifecycleはPHPTとdomain model reviewを必須にする。

runtime codeを変更する場合の最低gate:

- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- affected PHPUnit integration
- `./bench/run.sh cpu-micro --calls=20000 --warmup-calls=500`
- `./bench/run.sh metadata-header`
- parser/queue変更なら payload / streaming系benchmark
- HTTP/2/gRPC domain model review

docs-onlyの棚卸しではruntime benchmarkは不要である。

Runtime refactor(`RST_STREAM` submit helper集約)は `docs/issues/closed/2026-05-31-protocol-classification-runtime-boundary-refactor.md` で検討し、不採用にした。runtime codeは現状の直接呼び出しを維持する。
