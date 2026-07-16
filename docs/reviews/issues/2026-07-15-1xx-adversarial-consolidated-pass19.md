# 1xx informational response adversarial consolidated pass 19 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f` / `9401067` / `b17201d`（current HEAD）
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
- 仕様issue、rejected first attempt、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-5〜pass-17 consolidated review / gate records、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 19 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `b17201d`のempty-name invalid regular field classification、budget-first ordering、shared incomplete-header terminal action、exact raw fixture、production / diagnostic oracleを重点的に追跡した。
- issue Decision Logで受容済みの判断は再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: strict-invalid pseudo-headerがshared incomplete-header terminal actionを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）`
- Finding: 完了済みinformational responseの後、END_HEADERSなしの次blockでunknown pseudo-headerを受けると、nghttp2がapplicationのnormal / invalid-header callbackより先にstrict rejectする。この経路は`on_invalid_frame_recv_callback()`でcall-local `INTERNAL`を記録するだけで、`response_header_block_incomplete`とconnection terminal quarantineを設定しない。nghttp2がtarget `RST_STREAM(PROTOCOL_ERROR)`を送ってtarget callが有限終了しても、connection-global inbound HPACK decoderはCONTINUATION待ちのままであり、そのconnectionがpersistent cacheへ再利用可能な状態で残る。
- Evidence: productionのshared owner `grpc_protocol_apply_response_header_terminal_action()`はEND_HEADERSなしなら`response_header_block_incomplete`をsetし、`mark_connection_close_after_pending_flush()`でconnectionをdraining / close-after-flushへ遷移させる（`src/transport.c:2309-2323`）。しかしnghttp2 rejection observer `on_invalid_frame_recv_callback()`は`metadata_too_large` priorityを守った後、`response_header_protocol_error`とbody discardをsetするだけで同ownerへ接続しない（`src/transport.c:2591-2617`）。そのため`connection_usable()`は当該connectionをusableと判定し（`src/transport.c:915-941`）、reuse preflightもsocketにpending byteがなければtrueを返す（`src/transport.c:1210-1243`）。diagnosticの同observerもprotocol flagをsetするだけで`response_header_block_incomplete`を介したnonblocking finishへ接続しない（`src/diagnostic/bench.c:561-574`）。nghttp2 v1.69.0の[`nghttp2_http_on_header()` / response header validation](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_http.c#L428-L456)はunknown pseudo-headerを`NGHTTP2_ERR_HTTP_HEADER`としてapplication invalid-header callbackへ渡さず、[`inflate_header_block()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_session.c#L3528-L3615)はstream rejectionとinvalid-frame callbackへ進む。exact wire probeは `HEADERS(:status: 103, END_HEADERS)` → 同streamへ `HEADERS(END_STREAM=1, END_HEADERS=0, BlockFragment=00 04 3a 66 6f 6f 01 76)`（literal without indexing、`:foo: v`）→ CONTINUATIONを送らずTCPをopenのまま保持するsequenceである。
- Expected model: END_HEADERS未完了のinbound HPACK blockでterminal rejectionが確定した全producerは、application header callbackを通るかnghttp2 strict validationでrejectされるかにかかわらず、同じconnection-terminal lifecycleへ入る。nghttp2が既にtarget RSTをqueueしたinvalid-frame経路ではRSTを重複submitせず、call-local `INTERNAL`を維持したまま新規stream admission停止、best-effort control flush、connection dead化を行う。diagnosticも同じincomplete-block classificationを観測してdefault-blocking fdを有限終了可能にする。
- Why it matters: malformed peerは小さいheader fragmentとCONTINUATION省略だけで、target callを終えた後も壊れたHPACK stateを持つconnectionをcacheへ残せる。同一authorityのfollow-upはそのconnectionへadmitされて応答をdecodeできずhang / failureになり得て、既存siblingも合法なresponse HEADERSを受け取れない。これはpass-13で確立した「terminal failureが確定したEND_HEADERS未完了blockはconnection-terminal」というproduction lifecycle invariantを、別のnghttp2 rejection producerから破る。
- Recommended fix: `grpc_protocol_apply_response_header_terminal_action()`からincomplete-blockのconnection terminalizationを、RST submitとは独立した共有actionとして分離する。`on_invalid_frame_recv_callback()`がtarget HEADERSかつEND_HEADERSなしで、`metadata_too_large`によるlocal budget stopではない場合にそのactionを呼ぶ。nghttp2が既にqueueしたRSTは重複submitしない。diagnostic observerも同classificationからone-shot fdをnonblocking化する。上記raw controlを追加し、PHPT 042でunary / server streamingの有限`INTERNAL`、peer-observed exact `RST_STREAM(PROTOCOL_ERROR)`、sibling terminality、fresh connection follow-upを、PHPT 043でfailed-not-timedoutと実fd `O_NONBLOCK`を固定する。
- Fix summary: normal / recoverably-invalid field callbackをshared wire-header budget ownerの後に`STATUS` / `REGULAR` / `INVALID_REGULAR` / `REJECTED`へ分類し、全semantic phaseとのclosed route tableへ接続した。strict field rejectionとblock-end HTTP messaging rejectionは`REJECTED` default routeへ入り、nghttp2-owned RSTを重複submitせず、分離した`grpc_protocol_mark_response_header_terminal_action()`からEND_HEADERS未完了connectionをterminal quarantineへ移す。invalid-frame observerは`metadata_too_large`またはlocal TEMPORAL producerの既確定taxonomy / selected RSTを維持する。production / diagnosticは同じclassification / action helperを共有し、diagnosticは同じincomplete classificationからone-shot fdをnonblocking化する。C unitはdeclared / unknown field class × 全5 phaseをtable-driveし、raw fixture / PHPTはstrict-invalid `:foo`とuppercase regular nameをunary / server streaming / diagnosticで固定した。owning design docにexhaustive routing tableとbudget / RST / lifecycle ownershipを記載した。
- Fix commit: `pending`
- Verification: `変更済みtest-server imageをrebuild / force-recreate後、./tools/test/check-phpt.sh PASS（29/29、failed 0、skipped 0、warned 0）、./tools/test/check-c-unit.sh PASS（4/4）、docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（production / bench-enabled、findings none）。PHPT 042でstrict-invalid pseudo / uppercase regularのunary・server streamingに対する有限INTERNAL、exact PROTOCOL_ERROR、terminal connection、fresh follow-upを、PHPT 043でcallback bypass、failed-not-timedout、実fd O_NONBLOCKを確認した。HTTP/2 / gRPC domain model再レビューはBlocker / High / Medium / Low / Design Decisionすべてnone。`
- Notes: pass-13 `REVIEW-20260715-001`のconnection-terminal判断やpass-15 / pass-17のregular-before-status修正を再議論するものではない。application header callbackを通らないstrict-invalid pseudo-headerという未接続producerを示す新規findingである。`b17201d`のempty-name invalid regular field修正自体は、budget-first priorityとshared terminal actionを含めてadequateと確認した。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
