# 1xx informational response adversarial consolidated pass 23 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f` / `9401067` / `b17201d` / `2c9a61e` / `011547a`（current HEAD）
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
- 仕様issue、rejected first attempt、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-21 review / fix domain gate、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 23 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `011547a`のclosed / foreign streamに対するconnection-scope `on_begin_frame` observerを重点的に追跡し、late frame、persistent reuse、call unregister、stale pointer、production / diagnostic parityを確認した。
- issue Decision Logで受容済みの判断は再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: live streamで開始したfragmented HEADERSをdeadline/cancelで放棄するとconnection-terminal markerを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）`
- Finding: pass-21 `REVIEW-20260715-001` の修正は、HEADERS frame到着時点でlive `grpc_call`を持たないclosed / foreign streamだけをconnection-terminalにする。live streamでotherwise-validなHEADERS blockが開始し、END_HEADERSを受信する前にdeadlineまたはuser cancelでlocal RST_STREAMが送られる場合、block開始時のpredicateはlive callを理由にfalseとなり、その後のcancel / unregisterもopen blockをconnection lifecycleへ昇格しない。nghttp2のconnection-global inbound parserはCONTINUATION待ちのままstream user dataだけが外れ、persistent connectionが再利用可能として残る。
- Evidence: `src/transport.c:931-953` の `on_begin_frame_callback()` は `has_live_call == true` のHEADERSを、END_HEADERSなしでも明示的に除外する。`src/transport.c:2306-2335` はcall-local phaseを開始するが、validな `:status: 103` またはvalid initial fieldだけのfragmentではterminal actionを起動せず、HEADERS block完了時の `on_frame_recv_callback()` までphaseをendしない。peerがCONTINUATIONを保留してdeadlineを迎えると、unaryは `src/unary_call.c:214-231`、server streamingは `src/server_streaming_call.c:282-314` から `cancel_grpc_call_stream()`へ進む。同helper (`src/transport.c:350-389`) はRST_STREAMをsubmit / flushするだけで `response_header_phase.block_phase` やopen blockを検査せず、nghttp2のoutbound RST処理で `on_stream_close_callback()` (`src/transport.c:2679-2703`) がcallをunregisterしても `draining` / `close_after_pending_flush` はfalseのままである。nghttp2 v1.69.0の `nghttp2_session_mem_recv2()` はEND_HEADERSなしHEADERSの受信後にinbound stateを `NGHTTP2_IB_EXPECT_CONTINUATION`へ移し、outbound RST送信後の `nghttp2_session_close_stream()` はstreamを破棄するが `session->iframe` をresetしない。したがってsocketがsilentならpersistent preflightはEAGAINをusableとし、次streamを同じdecoderへadmitする。exact probeは stream 1へ `HEADERS(:status: 103, END_HEADERS=0, END_STREAM=0)` を送り、CONTINUATIONを送らずTCPをopenに保つ。短いdeadlineのtargetは `DEADLINE_EXCEEDED` + peer-observed `RST_STREAM(CANCEL)`となるが、current codeはconnectionをcacheへ残す。続く同一client RPCはfresh prefaceではなく同一connectionへsubmitされ、peerが新streamのHEADERSを返すとexpected CONTINUATION違反でconnection failureとなるか、peerがsilentならdeadlineなしで停止する。server-streamingのuser-cancel probeはcomplete initial HEADERS + 1 DATA messageの後にvalid-so-farなterminal HEADERS（END_STREAM=1 / END_HEADERS=0、regular trailing metadataだけ）を送り、最初のmessage yield後に`cancel()`する形で同じ遷移を再現できる。
- Expected model: inbound header blockのopen / complete lifecycleはconnection scopeに属する。active callがownerだったfragmented blockでも、`grpc_response_header_phase_end()`前にdeadline、user cancel、destructor等でstreamをlocal close / unregisterする場合は、target callのprimary taxonomyとcaller-selected RST codeを維持したまま既存のincomplete-header connection terminal actionへ昇格し、新規admission停止、bounded RST/control flush、dead connection破棄へ収束させる。
- Why it matters: hostileまたは壊れたpeerは、valid-so-farな小さいHEADERS fragmentを1つ送ってCONTINUATIONを保留するだけで、targetのstream-local deadline/cancel後にpersistent connectionをpoisonできる。target自体は期待どおり終了しても、無関係な後続RPCが失敗または停止するため、pass-21で守ろうとしたconnection reuse invariantがlive-to-closed transitionでは成立しない。UAFではないが、production unary / server streamingのavailabilityとfailure scopeが崩れる。
- Recommended fix: call-local phaseが`NONE`でないこと等からopen inbound response blockを明示的に追跡し、`cancel_grpc_call_stream()`または全local close / unregister共通ownerで、open blockを放棄する前に `mark_connection_close_after_pending_flush()` 相当へ昇格する。deadline / CANCELLEDのstatus priorityとexact target RSTを変えず、flush後はconnectionをdead化する。raw fixture / PHPT 042へ上記unary deadlineとserver-streaming explicit cancel / destructorを追加し、target status、exact CANCEL、dead connection destroy、同一clientのfresh-preface follow-up成功を固定する。C unitは現在の瞬間的な `(is_headers, end_headers, has_live_call)` 8通りに加え、`live begin -> block open -> local close before CONTINUATION` のtemporal transitionを固定する。
- Fix summary: `response header phaseへblock_phase != NONEをopen inbound blockとみなすpure predicateを追加し、unary / server-streaming deadline、explicit cancel、resource destructor、stream-local semantic-error teardownが収束するcancel_grpc_call_stream()でRST submit前に既存incomplete-header connection terminal actionへhandoffした。complete rejected HEADERSはproduction / diagnosticのinvalid-frame callbackでphaseをNONEへ戻し、完了済みblockの後続cancelをfalse positiveにしない。raw diagnosticは同じpredicateをpoll-loop failure時に適用し、deadlineではexact RST_STREAM(CANCEL)をnonblockingでbest-effort flushする。raw fixture、PHPT 042 / 043、C unit、design / verification docsでproduction / diagnostic parity、target taxonomy、dead connection、fresh follow-upを固定した。`
- Fix commit: `pending`
- Verification: `poc/test-server/main.goの変更を含むtest-serverをdocker compose up -d --build --force-recreate test-serverでrebuild / restartしてPASS。./tools/test/check-phpt.sh PASS（29/29 tests、failed 0、skipped 0、warned 0）。./tools/test/check-c-unit.sh PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。PHPUnit PASS（31 tests / 116 assertions）。./tools/test/check-c-static-analysis.sh PASS（production / bench-enabled findings none）。PHPT 042はlive informational fragment中のunary deadlineとlive trailing fragment中のserver-streaming explicit cancelについてprimary status、exact CANCEL、dead connection destroy、same-client fresh-preface follow-upを確認し、PHPT 043はshared predicate、session-terminal marker、nonblocking finite finishを確認した。HTTP/2 / gRPC domain model reviewはBlocker / High / Medium / Lowすべてnone、Design Decision 1件Accepted（docs/reviews/issues/2026-07-16-pass23-abandonment-domain-review.md）。`
- Notes: pass-21のaccepted decisionである「active fragmented HEADERSはcall-local routeがowner」を再議論するものではない。本findingは、そのownerがblock completion前にdeadline / cancelで消えるtemporal transitionがconnection lifecycleへhandoffされない修正不足を、旧IDを参照した新規findingとして扱う。PHPT 043のraw batchはtimeout failure後にone-shot fd / sessionを閉じるため、persistent reuseのruntime impactはproduction PHPT 042側が主対象となる。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
