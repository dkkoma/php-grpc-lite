# 1xx informational response adversarial consolidated pass 21 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f` / `9401067` / `b17201d` / `2c9a61e`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport_core.[ch]`
- `src/status_core.c`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- 仕様issue、rejected first attempt、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-5〜pass-19 consolidated review / gate records、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 21 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `2c9a61e`の`STATUS` / `REGULAR` / `INVALID_REGULAR` / `REJECTED` field class × phase routing table、nghttp2 callback producerの接続、budget-first ordering、production / diagnostic parityとtable-driven C-unit exhaustivenessを重点的に追跡した。
- issue Decision Logで受容済みの判断は再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: closed-streamのlate incomplete HEADERSがfield routingを通らずpersistent connectionをpoisonする

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）`
- Finding: `2c9a61e`はactive callへ公開またはstrict rejectされたheader fieldをclosedなclass × phase tableへ接続したが、正常なterminal responseで既にclose / unregisterされたstreamへ後着するHEADERS blockはそのproducer集合に入らない。nghttp2はclosed local streamのHEADERSを`NGHTTP2_ERR_IGN_HEADER_BLOCK`としてapplication header callbackなしでHPACK decodeし、END_HEADERSがなければconnection-global decoderをCONTINUATION待ちに残す。productionはこのblockを観測せずconnectionをusableのままcacheへ戻すため、同一authorityのsibling / follow-upを壊れたsessionへadmitできる。
- Evidence: `on_stream_close_callback()`はvalid terminal responseで`call->stream_closed`をsetした直後に`unregister_grpc_call_stream()`を呼び、nghttp2 stream user dataとactive-stream registrationを外す（`src/transport.c:147-175,2652-2677`）。current terminal owner `grpc_protocol_mark_response_header_terminal_action()`はlive `grpc_call *`を入口としてEND_HEADERSなしなら`response_header_block_incomplete`と`draining` / `close_after_pending_flush`を設定するが（`src/transport.c:2309-2321`）、`on_begin_headers_callback()`、normal / invalid header callback、`on_invalid_frame_recv_callback()`はいずれもcall lookupまたはnghttp2からのcallback deliveryを前提とする（`src/transport.c:2279-2307,2445-2649`）。nghttp2 v1.69.0の[`session_process_headers_frame()` / `nghttp2_session_on_request_headers_received()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_session.c#L3854-L4045)は、既に削除されたclient-local stream idをidle streamとはせず`NGHTTP2_ERR_IGN_HEADER_BLOCK`へ落とす。その後[`inflate_header_block()` / inbound state machine](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_session.c#L3528-L3615)は`call_header_cb=false`でHPACK stateだけを進め、fragment完了後は`NGHTTP2_IB_IGN_CONTINUATION`となるため、normal / invalid-header / invalid-frame / frame-recv callbackのどれも発火しない。productionはraw `on_begin_frame`相当も登録していない（`src/transport.c:428-443`）。late fragmentを最初のresponseと同じ`nghttp2_session_mem_recv()`で消費するとsocketにpending byteが残らず、`connection_usable()`はfd / session / dead / drainingだけを検査し（`src/transport.c:929-932`）、plaintext reuse preflightは`MSG_PEEK`のEAGAINでtrueを返す（`src/transport.c:1211-1230`）。diagnostic batchも1 sessionをiterations間で再利用する一方、各iteration冒頭でcall-local phase / incomplete flagをresetする（`src/diagnostic/bench.c:1600-1720`）ため、同じ見えないCONTINUATION待ちを次iterationへ持ち越す。exact runtime probeは、stream 1へ `HEADERS(:status: 103, END_HEADERS)` → valid `HEADERS(:status: 200, content-type: application/grpc, END_HEADERS)` → valid gRPC DATA → `HEADERS(grpc-status: 0, END_HEADERS|END_STREAM)` → 同じTCP write内でclosed stream 1へ `HEADERS(END_STREAM=1, END_HEADERS=0, BlockFragment=00 04 3a 66 6f 6f 01 76)`（literal without indexing、`:foo: v`）を送り、CONTINUATIONなしでTCPをopenのまま保持するsequenceである。
- Expected model: inbound HPACK blockのlifecycle ownerはcall registrationより長くconnectionに属する。active targetだけでなくclosed / unowned streamへ到着してnghttp2がapplication field callbackから隠すHEADERSについても、terminally invalidかつEND_HEADERS未完了であることをconnection scopeで観測し、新規admission停止と有限なconnection teardownへ遷移する。既に完了したcallのOK taxonomyは書き換えず、late frameを`current_read_call`やsiblingへ誤帰属せず、stale `grpc_call *`もdereferenceしない。
- Why it matters: malformed peerはvalidなRPCを成功終了させた後、9-byte frame headerと小さいHPACK fragmentだけでpersistent connectionをCONTINUATION待ちにpoisonできる。follow-up request自体は送信できるが、その合法なresponse HEADERSはexpected CONTINUATION違反になりconnection failureとなるか、peerがsilentならdeadlineなしcallを保持する。diagnostic batchでも次iterationが同じsessionへ進み、productionと同型のhang / failureになる。これはpass-19 `REVIEW-20260715-001`で確立したconnection-terminal invariantを、call unregister後にcallback deliveryを持たないproducerから再び迂回する。
- Recommended fix: nghttp2のfield / invalid-frame callbackより外側に、recv bufferを跨ぐframe-header stateを持つconnection-scope observer、または`NGHTTP2_ERR_IGN_HEADER_BLOCK`でも確実に発火することを確認した同等hookを設け、unowned / closed streamの`HEADERS && !END_HEADERS`をcall pointerなしでterminal quarantineへ写像する。通常のactive streamに対する合法なfragmented HEADERSは除外する。callback / observer内ではconnectionのadmission停止とclose-after-flush markerだけを設定し、RST重複submit、flush、detach、destroy、`nghttp2_session_del()`は行わず、`nghttp2_session_mem_recv()`復帰後に既存ownerへ処理させる。上記same-write raw controlをfixtureへ追加し、PHPT 042でunary / server streamingの先行callがexact OKのまま、直後の同一authority follow-upがfresh prefaceで有限OKとなることを固定する。PHPT 043は2 iterationsで1件目のOKを保存し、2件目を開始せずtimeoutなしでconnection-terminal failureへ収束するoracleを追加する。通常の`on_begin_frame` callbackはnghttp2のignored-header-block stateで抑止され得るため、採用前にこのexact sequenceでdeliveryを確認する。
- Fix summary: `field-class × call phase tableはlive callのsemantic routingとして変更せず、connection/session scopeのshared pure predicate（HEADERSかつEND_HEADERSなし、live callなし）を追加した。production / diagnostic双方でnghttp2のon_begin_frame callbackを登録し、ignored closed / foreign streamのfield callback抑止前にframe headerを観測する。productionは完了済みcallへ触れず既存のclose-after-pending-flushへ遷移し、diagnosticはiteration reset外のsticky terminal markerで2件目をsubmit前に停止する。raw fixtureはvalid 103→200→DATA→grpc-status 0とlate incomplete HEADERSを1回のTCP writeで送り、PHPT 042 / 043とC unitでlifecycle / predicateを固定した。`
- Fix commit: `pending`
- Verification: `nghttp2 v1.69.0のsource / exact local probeで、closed streamのHEADERSでもon_begin_frameがIGN_HEADER_BLOCK判定前に発火し、同じmem_recv内のterminal close後にはstream user dataがNULLであることを確認。test-server rebuild / restart PASS。./tools/test/check-phpt.sh PASS（29/29）。./tools/test/check-c-unit.sh PASS（4/4群）。PHPUnit PASS（31 tests / 116 assertions）。./tools/test/check-c-static-analysis.sh PASS（production / bench-enabled findings none）。PHPT 042はunary / server streaming先行callのOK、RSTなし、dead connection destroy、fresh follow-upを、PHPT 043はok=1 / failed=1 / submitted=1 / timeoutなしを確認。HTTP/2 / gRPC domain model reviewはBlocker / High / Medium / Low / Design Decisionすべてnone（docs/reviews/issues/2026-07-15-pass21-late-closed-stream-domain-review.md）。`
- Verification addendum (2026-07-16): `routine QA finalizationでtest-serverをrebuild / force-recreateし、PHPT 29/29、C unit 4/4群、PHPUnit 31 tests / 116 assertions、production / bench-enabled static analysis findings noneを再確認した。Fix summary、fixture / test oracle、issue bookkeepingはcurrent working treeと一致し、追加の実装修正は不要。`
- Notes: pass-19 `REVIEW-20260715-001`の修正がactive callのstrict-rejection producerには十分であること、field class × phase tableとC-unitがdeclared class集合を網羅することは確認した。本findingはその判断を再議論せず、fixがcall registration lifetime外のconnection-global HPACK producerを閉じていないことを示す新規edgeである。C safety上の問題はUAFではなく、callbackが安全に`NULL` / non-deliveryとなる結果としてconnection terminal classificationを失うことである。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
