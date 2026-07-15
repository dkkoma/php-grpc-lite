# pass-23 abandonment HTTP/2/gRPC domain model review 2026-07-16

## Scope

- `src/response_header_phase.[ch]`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- pass-23で変更されたdesign / code-reading / verification docs

## Reviewer Role

- HTTP/2 / gRPC lifecycle・production / diagnostic boundary reviewer

## Review Prompt Summary

- `REVIEW-20260715-001` の修正について、live callが所有するfragmented response HEADERSをdeadline、explicit cancel、resource destructor、stream-local semantic-error teardownで放棄する際のconnection-terminal handoffを追跡した。
- call / stream / connection scope、primary statusとRST ownership、fatal error後のlifecycle、production persistent connectionとraw diagnostic one-shot sessionの責務境界、fixture / PHPT / C unitの識別力を確認した。
- 実装変更は行わず、親エージェントから4 suiteすべてgreenとの最終結果を受領した。

## Issues

### REVIEW-20260716-001: local abandonmentのhandoffをfield routeではなく共通teardown seamに置く

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC lifecycle・production / diagnostic boundary reviewer`
- Finding: 問題となる設計境界は、live callの消失をresponse field classificationへsynthetic rowとして追加するか、local close pathが収束するteardown seamでconnection lifecycleへhandoffするかである。本修正は後者を採用し、`block_phase != NONE`だけをshared pure predicateとした。この配置は意図的で、現行docsに明記されている。
- Evidence: `src/response_header_phase.c:49-55` の `grpc_response_header_phase_requires_connection_terminal_on_abandonment()`、`src/transport.c:354-388` の `cancel_grpc_call_stream()`、`src/transport.c:2346-2387` のincomplete-block mark / rejected complete-block phase finish、`src/diagnostic/bench.c:328-371` のraw diagnostic consumer。`docs/SPEC.md`、`docs/design/grpc-call-exchange-state.md`、`docs/design/protocol-classification-boundary.md`、`docs/guides/code-reading-guide.md` はscope別consumerとfailure taxonomy非所有を説明している。
- Expected model: inbound fragmented HEADERSのHPACK completion可否はHTTP/2 Connection scopeのinvariantである。live callがblockを所有中にlocal abandonmentする場合、call-local statusとcaller-selected RST codeを変更せず、call pointerを失う前にconnectionへterminal decisionをhandoffする。fatal I/O / nghttp2 error後はconnectionが既にterminalであり、unregister / owner clearはsessionを再駆動しないlocal bookkeepingに限る。diagnosticは同じclassificationを共有しつつ、one-shot session固有の有限teardownだけを所有する。
- Why it matters: field routeへabandonmentを混ぜるとwire field classificationとPHP/resource lifecycleが結合する一方、unregisterへ判断を遅らせるとopen block ownerを失う。共通cancel seamでのhandoffはunary deadline、server streaming explicit cancel / destructor、semantic-error teardownを同じconnection invariantへ収束させ、対象callの`DEADLINE_EXCEEDED` / `CANCELLED`等とexact RSTを保存する。
- Recommended fix: 追加修正なし。今後local stream close pathを追加する場合も `cancel_grpc_call_stream()` へ収束させるか、同じopen-block handoffをowner消失前に明示する。complete rejected HEADERSで `block_phase` を `NONE` に戻すfallbackを維持し、pure predicateがcomplete blockを誤ってterminal化しないことを守る。
- Fix summary: `grpc_response_header_phase_requires_connection_terminal_on_abandonment()` と `cancel_grpc_call_stream()` のhandoff、productionのbounded RST/control flush後のdead化、diagnosticのsession-terminal markerとnonblocking best-effort CANCELが実装され、design / verification docsへ現在のrouting modelとして記録済み。
- Fix commit: `pending`
- Verification: `tests/unit/test_response_header_phase.c:206-226` がphase predicateのopen / end遷移を固定する。PHPT 042はunary deadlineとserver streaming explicit cancelについてprimary status、exact CANCEL、dead connection destroy、fresh-preface follow-upを固定し、PHPT 043はdiagnostic timeoutのCANCEL、session-terminal marker、nonblocking finite finishを固定する。親エージェントから 2026-07-16 にPHPT、C unit、PHPUnit、C static analysisの4 suiteすべてgreenとの報告を受領した。domain model再レビューではBlocker / High / Medium / Lowなし。
- Notes: teardown seamはfield-class tableを拡張せず、wire classificationとcall/resource lifecycleを分離する既存モデルの明示的な延長として受容する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 (Accepted)`
