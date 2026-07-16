# 1xx informational response adversarial consolidated pass 25 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f` / `9401067` / `b17201d` / `2c9a61e` / `011547a` / `573b101`（current HEAD）
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
- `tests/unit/test_transport_core.c`
- 仕様issue、rejected first attempt、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-23 review / fix domain gate、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 25 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `573b101`について、deadline、explicit cancel、resource destructor、semantic-error teardown、fatal I/Oからowner clearまでを追跡し、live response header blockのconnection-terminal handoffとpersistent reuse decisionを重点確認した。
- issue Decision Logで受容済みの判断は再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: HEADERS frame payload受信途中のabandonmentはphase開始前なのでconnection-terminal handoffを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）`
- Finding: pass-23 `REVIEW-20260715-001` の修正は、`on_begin_headers_callback()` が設定する `block_phase != NONE` をopen inbound header blockの唯一の判定にしている。しかしnghttp2は9-byte frame header受信時に `on_begin_frame_callback()` を呼び、HEADERS control frame全体を受信してvalidityを判定した後に `on_begin_headers_callback()` を呼ぶ。live streamのHEADERS frame headerを受信した後、宣言されたframe payloadの途中でpeerが停止すると、connection parserはHEADERS frame内にいる一方でcallの `block_phase` はまだ `NONE` のままである。このwindowでdeadline / local cancelが起きると、`grpc_protocol_mark_abandoned_response_header_block()` はfalseとなり、stream RSTだけを送ってpartial frameを保持したconnectionを再利用可能なまま残す。
- Evidence: `src/transport.c:940-961` の `on_begin_frame_callback()` はlive callを持つHEADERSをEND_HEADERSの有無にかかわらずconnection-terminal predicateから除外する。call-local ownerが始まるのは `src/transport.c:2315-2325` の `on_begin_headers_callback()` であり、`src/response_header_phase.c:49-55` と `src/transport.c:2367-2372` のabandonment predicateはその後の `block_phase` しか見ない。`cancel_grpc_call_stream()` (`src/transport.c:354-391`) はpredicateがfalseなら `close_after_pending_flush` を設定せず、RST flush成功後もconnectionをdead化しない。nghttp2の公式receive contractは、[`on_begin_frame_callback`をframe header受信時に呼ぶ](https://nghttp2.org/documentation/nghttp2_session_callbacks_set_on_begin_frame_callback.html)一方、control frameは全体を受信し、validなHEADERSの場合に初めて[`on_begin_headers_callback`を呼ぶ](https://nghttp2.org/documentation/nghttp2.h.html)順序を定義する。従ってexact probeは、live stream 1へ「payload lengthを正数にしたHEADERS frame header（END_HEADERSなし）」だけ、またはpayloadの一部だけを送って残りを保留し、短いdeadlineを発火させるsequenceである。current codeではtargetは `DEADLINE_EXCEEDED` + `RST_STREAM(CANCEL)`になってもconnectionはcacheに残る。続く同一client RPCのresponse bytesは旧HEADERS payloadの残りとして消費され、失敗または停止する。fixture probeではtargetのexact CANCEL、dead connection destroy、same-client follow-upのfresh prefaceを独立にassertすべきである。
- Expected model: inbound HEADERS ownershipはframe header受信から始まり、frame payload / HPACK blockが完全に終了するまでconnection reuse decisionを覆う。live callの `on_begin_frame` 後、`on_begin_headers` がsemantic phase ownershipを引き取る前にcallを放棄する場合も、primary statusとcaller-selected RST codeを維持したままconnection-terminal actionへhandoffする。complete frameのvalid / rejected callbackへ到達した場合だけpre-phase markerをclearし、END_HEADERS未完了blockは既存phase ownerへ継続させる。
- Why it matters: hostileまたは壊れたpeerはHEADERS frame headerと少量のpayloadだけでtarget deadline後のpersistent connectionをpoisonできる。target call自体は期待したstatusで終わるが、無関係なfollow-up RPCがpartial-frame parserへ送られて失敗または停止するため、`573b101` が閉じるとしたlive-to-closed teardown failure classがframe受信境界では未完了である。
- Recommended fix: live-call HEADERSを観測した `on_begin_frame_callback()` から `on_begin_headers_callback()` までを表すcall/connection-local markerを追加し、`cancel_grpc_call_stream()` のabandonment判定を `pre-phase HEADERS frame in progress || block_phase != NONE` とする。`on_begin_headers_callback()` がphase ownershipを開始した時点でpre-phase markerをclearし、complete rejection / fatal parser errorでもscopeに応じてclearまたはconnection terminal化する。raw fixtureへHEADERS frame headerだけを送ってpayloadを保留するunary deadline（可能ならserver-streaming explicit cancelも）を追加し、target status、exact CANCEL、dead connection destroy、fresh-preface follow-upを固定する。
- Fix summary: `call abandonment後のconnection reuse判定をsemantic phaseからconnection/session-scopedなreceive byte-boundary invariantへ移した。nghttp2が正常にconsumeしたraw bytesからpartial 9-byte frame header、frame payload残量、HEADERS / CONTINUATION block継続を追跡し、clean frame boundaryかつheader block非継続の場合だけgrpc_h2_receive_allows_reuse_after_abandonment()がtrueを返す。production persistent connectionとraw diagnostic one-shot sessionは同じpure tracker / predicateをsession lifetimeで共有し、phaseはfield semanticsのownerに限定した。raw fixture / PHPTへfield callback前で止まるpartial HEADERS payload deadlineと、complete HEADERS後のclean-boundary same-connection reuse controlを追加し、C unitでbyte fragmentationとpredicate matrixを固定した。`
- Fix commit: `pending`
- Verification: `2026-07-16にtest-serverをrebuild / restartした。PHPTは29/29 PASS（failed / skipped / warned 0）、C unitは4/4 group PASS、PHPUnitは31 tests / 116 assertions PASS、C static analysisはproduction / bench enabledの両方でexit 0（findingなし）。PHPT 042のpartial-headers-payload-deadlineはDEADLINE_EXCEEDED、exact RST_STREAM(CANCEL)、旧connection dead化とfresh-preface follow-upを、clean-headers-boundary-deadline / require-prior-clean-boundary-cancelは同一connection reuseを固定する。transport_core C unitはpartial frame header、partial HEADERS / DATA payload、HEADERS→CONTINUATION、zero-length frame、multiple frame / chunk splitとNULL fail-closedを固定する。production / diagnosticの全nghttp2_session_mem_recv() consumerが同じtrackerを更新することを静的照合し、fix後domain model re-reviewでBlocker / High / Medium / Lowはいずれもnone。`
- Notes: `pass-21でAcceptedとなった「active fragmented HEADERSはcall-local owner」という判断や、pass-23のpost-on_begin_headers修正を再議論するものではない。pass-23 findingのfixが、その直前に存在するframe-header-received / semantic-phase-not-started transitionを覆わない点を新しいIDで記録する。`

### REVIEW-20260716-002: persistent preflightがpartial frame headerをclean boundaryとしてadmitし得る

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `call abandonment時点がclean boundaryでも、その後inactive persistent connectionのpreflight drainが1〜8 byteの次frame headerだけを受信してEAGAINへ到達すると、receive boundary predicateがfalseのままconnectionを再利用可能として返す。nghttp2は9-byte frame header完成前にはframe callbackを発火しないため、on_begin_frameのlate unowned HEADERS terminal routeもこの状態を観測できない。`
- Evidence: `src/transport.c:1141-1220 の drain_pending_connection_data_for_reuse() はconnection_session_mem_recv()でreceive_boundaryを更新した後、read boundary到達時にconnection_usable()だけを返す。src/transport_core.c:179-188 / 203-212は1〜8 byteのframe headerをframe_header_bytesへ保持し、grpc_h2_receive_allows_reuse_after_abandonment()をfalseにするが、preflight終端はこのpredicateを参照しない。tests/unit/test_transport_core.c:150-180はpure predicateのfail-closedを固定する一方、persistent preflight consumerとの接続は固定していない。`
- Expected model: `abandonment後のinactive HTTP/2 connectionは、adoption直前のpreflightを含め、receive parserがcomplete frame間にありheader block継続もない時だけ新規streamをadmitする。preflight自身がraw bytesをconsumeしてpredicateをfalseへ遷移させた場合は、そのconnectionをcache reuseから外しfresh connectionへフォールバックする。`
- Why it matters: `peerはclean-boundary CANCEL後、次callのpreflightへframe headerのprefixだけを渡して停止できる。current codeはparser途中のconnectionへfollow-up streamをsubmitするため、後続response bytesが旧frame header / payloadとして解釈され、無関係なRPCが停止または失敗し得る。これはpass-25が導入したbyte-level reuse invariantのconsumer漏れである。`
- Recommended fix: `inactive persistent preflightのdrain終端でgrpc_h2_receive_allows_reuse_after_abandonment(&connection->receive_boundary)を確認し、falseならconnectionをdraining/deadとしてcache reuseを拒否する。raw fixture / PHPTにはclean-boundary abandonment後、follow-up preflightへ1〜8 byteのframe headerだけを送ってfresh connectionへフォールバックするprobeを追加し、pure predicateだけでなくproduction adoption seamを固定する。あわせてpartial DATA等にも適用する現在のpredicateに合わせ、connection terminal helper / error detail / lifecycle commentの「incomplete HPACK/header block」限定表現をunclean receive frame boundaryへ更新する。`
- Fix summary: `preflight_persistent_connection()はinactive connectionのdrain後、connection_usable()に加えてgrpc_h2_receive_allows_reuse_after_abandonment()を再確認する。drainがpartial frame header / payloadまたはopen header blockで終了した場合はconnectionをdead化し、cache entryを除去してfresh connectionへフォールバックする。raw fixture / PHPTにはclean-boundary CANCEL後に次frame headerの先頭8 byteだけを旧connectionへ送り、別authorityのbarrierでkernel送信queueまで到達したことを確認してからfollow-up preflightを実行するprobeを追加した。targetのexact CANCEL、preflightの8-byte read、旧connection dead化、fresh preface、persistent_reused=falseを固定する。`
- Fix commit: `pending`
- Verification: `2026-07-16のPHPT 042 late-partial-frame-header-after-clean-cancel probeで、targetはclean boundaryでexact RST_STREAM(CANCEL)を送り、barrier成功後のfollow-up preflightが旧connectionからexact 8 byteを読み、旧connectionをdead化してfresh connectionへフォールバックすることを確認した。PHPT 29/29、C unit 4/4 group、PHPUnit 31 tests / 116 assertions、C static analysis production / bench enabledはいずれもPASS。fix後domain model re-reviewでadoption seamとbyte-level predicateの対応を再確認し、追加findingなし。`
- Notes: `call teardown時のpartial HEADERS payloadとclean-boundary same-connection reuseを固定した既存PHPT / C unitの識別力は維持する。本指摘はclean boundary後にpreflight自身がreceive stateをdirtyへ遷移させるadoption edgeを対象とする。`

### REVIEW-20260716-003: callback-owned semantic RSTがdirty receive-boundaryをconnection lifecycleへ渡さない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `receive callback内でmessage size、compressed flag、malformed gRPC frame、final response前DATA等のsemantic failureを確定する経路はgrpc_protocol_submit_rst_stream_in_callback()からRST_STREAMを直接queueするが、connection/session-scoped receive-boundary predicateを確認しない。socket readがDATA frame payload途中で終わった場合、trackerはdirtyでも同callbackが利用可能なpayload prefixをsemantic parserへ渡すため、RST送信とstream close後にcancel_grpc_call_stream()へ到達せずpartial frameを持つconnectionが再利用可能なまま残る。`
- Evidence: `src/transport.c:1047-1071 の grpc_protocol_submit_rst_stream_in_callback() はfatal submitだけconnectionをdead化し、non-fatal RSTではreceive_boundaryを見ない。src/transport.c:2302-2329 の on_data_chunk_recv_callback()、3358-3402 のunary message parser、3427-3514 / 3576-3587 のserver-streaming parser / read-ahead limitはsemantic flagをsetして同helperへ直接入る。src/unary_call.c:233-260はmem_recv後にqueued RSTをsendし、on_stream_closeでloopを終えるためcommon local-abandonment seamを再実行しない。server streamingもsrc/server_streaming_call.c:317-346でRSTをflushした後、semantic flagによるterminate処理へ到達した時点ではstream close済みとなり得る。raw diagnosticのbench_on_data_chunk_recv_callback()もshared parserまたはdirect nghttp2_submit_rst_stream()を使い、header terminal wrapperを通らない。`
- Expected model: `stream-local semantic failureのtaxonomyとRST codeはcall ownerが維持する一方、callback内でcall ownershipを終わらせる全RST producerは、RST submit前に同じconnection/session receive-boundary predicateを適用する。current mem_recv invocationがpartial frame header / payloadまたはopen header blockで終わるなら、productionはpending RST/control flush後にconnectionをterminal化し、diagnosticはiterationを跨がないsession-terminal markerへ写像する。`
- Why it matters: `peerはDATA frameの先頭にoversize length、invalid compressed flag、またはfinal response前payloadを置き、frame payloadの残りを保留できる。clientは期待するstream-local statusとRSTを返しても、partial DATA frameをpersistent sessionへ残し、follow-up response bytesをそのpayload残りとしてconsumeして無関係なRPCを停止または失敗させ得る。pass-25 invariantがdeadline / explicit cancel seamだけに適用され、semantic callback producerでは成立していない。`
- Recommended fix: `callback-owned RSTの共通submit seamでdirty receive-boundaryをgeneric connection/session terminal actionへhandoffし、header固有taxonomyやRST ownershipと分離する。production / diagnostic双方でidempotentに適用し、partial DATA payload内でsemantic rejectionを発火させるraw fixtureをunaryとserver streamingの代表経路へ追加して、primary status、exact RST、dead/fresh connection、diagnostic iteration停止を固定する。`
- Fix summary: `grpc_protocol_submit_rst_stream_in_callback()はnon-fatalなcallback-owned semantic RSTをqueueする前にconnectionのreceive-boundary predicateを確認し、dirtyならmark_connection_close_after_pending_flush()へhandoffする。call固有のstatus、details、RST codeは変更しない。raw fixture / PHPTにはcomplete response HEADERS後、DATA frame payloadを1 byte残した状態でunsupported compressed-message rejectionを発火するunary / server-streaming probeを追加し、INTERNAL、details、exact RST_STREAM(CANCEL)、旧connection dead化、fresh follow-upを固定した。raw diagnosticのsemantic failureはone-shot batch / sessionをその場で終了してfd / sessionを破棄し、dirty sessionを次iterationへ再利用しないため、追加のdiagnostic markerは不要と判断した。`
- Fix commit: `pending`
- Verification: `2026-07-16のPHPT 042 partial-data-compressed-message probeで、unary / server streamingともcompleted inbound HEADERSは1、completed DATAは0、statusはINTERNAL、detailsはcompressed gRPC messages are not supported、target methodへのRST_STREAMはexact CANCELであることを確認した。旧connectionはfollow-up前にdead化され、follow-upはfresh connectionで成功する。PHPT 29/29、C unit 4/4 group、PHPUnit 31 tests / 116 assertions、C static analysis production / bench enabledはいずれもPASS。fix後domain model re-reviewでcallback RST seam、connection / stream scope、status / details / RST ownershipを再確認し、追加findingなし。`
- Notes: `response header field rejectionはgrpc_protocol_apply_response_header_terminal_action()が既にpredicateを適用するため、本指摘はそのwrapperを通らないDATA / shared message parser / read-ahead等のsemantic RST producerを対象とする。`

### REVIEW-20260716-004: connection-terminal diagnosticsがHPACK incompleteへ限定されたまま

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `close-after-pending-flush actionはpartial frame headerや任意frame payloadにも適用されるgeneric dirty receive-boundary consumerへ拡張されたが、error detailと近傍commentは「incomplete response header block」「inbound HPACK decoder incomplete」「missing CONTINUATION」のままである。partial DATA等によるterminal化をheader failureとして報告し、実装上の責務もheader専用に見せる。`
- Evidence: `src/transport.c:929-939 の mark_connection_close_after_pending_flush() とlast_error_detail、src/transport.c:2109-2127 のterminal flush後dead化comment、2145-2150 のflush_terminal_quarantine() comment、2205-2209 のDATA defer comment、2366-2377 のgrpc_protocol_mark_incomplete_response_header_block() commentはいずれもHPACK / response header block限定で記述される。一方src/transport_core.c:203-212のpredicateはframe_header_bytesまたは任意frame_payload_remainingでもfalseとなり、src/transport.c:373-378のcancel seamはframe typeを問わず同actionを呼ぶ。`
- Expected model: `connection lifecycleのdiagnosticと命名は、semantic response-header phaseではなく「unclean inbound HTTP/2 receive boundary」という実際のdomain conceptを表す。header block継続はその一例として説明し、partial DATA / control frameも誤分類しない。`
- Why it matters: `sibling callのUNAVAILABLE detailsとtraceが誤ったfailure classを示し、運用診断と将来のlifecycle変更でheader semanticsとgeneric receive parser stateを再び混同しやすい。runtime safetyはMedium findingsとは独立だが、pass-25の責務分離を文言が反映していない。`
- Recommended fix: `error detailとlifecycle commentsを「incomplete/unclean HTTP/2 receive frame boundary」等のgeneric表現へ更新し、必要ならheader固有のcall markerとgeneric connection terminal actionの名前を分離する。public status taxonomyやRST codeは変更しない。`
- Fix summary: `generic connection terminal helperのlast_error_detailとlifecycle commentをincomplete HTTP/2 receive boundaryへ更新し、partial frame header / payloadとopen header blockを同じconnection-level conceptとして表現した。current design docsにはcancel_grpc_call_stream()、grpc_protocol_submit_rst_stream_in_callback()、preflight_persistent_connection()の3 enforcement seamと、callback-owned semantic RST / inactive preflightのlifecycleを追記し、response-header field semanticsとgeneric receive-boundary ownershipを分離した。public status taxonomyとRST codeは変更していない。`
- Fix commit: `pending`
- Verification: `2026-07-16にcurrent implementation / design docsをrgで再確認し、incomplete HTTP/2 response header block、inbound HPACK decoder incomplete、missing CONTINUATION、HPACK block made the connection terminalのgeneric terminal-action残存表現がないことを確認した。header固有の現在も有効な説明と過去review recordは維持した。PHPT 29/29、C unit 4/4 group、PHPUnit 31 tests / 116 assertions、C static analysis production / bench enabledはいずれもPASS。fix後domain model re-reviewで命名、責務、connection / stream / call scope、lifecycle、production / diagnostic boundaryを再確認し、追加findingなし。`
- Notes: `過去review record内の当時正しかったHPACK限定記述は履歴として変更対象にしない。current implementation / design docsの現在モデルだけを揃える。`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
