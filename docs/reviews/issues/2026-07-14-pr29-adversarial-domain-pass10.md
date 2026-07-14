# PR #29 第十パス HTTP/2 / gRPC domain model review 2026-07-14

## Scope

- `76d282766091de75bd670f00cd05df8ef264335f..f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`
- `docs/SPEC.md` §4.2
- `src/transport.c`（`cancel_grpc_call_stream()` / `send_pending_h2_frames_with_deadline()` / preflight drain）
- `src/unary_call.c` / `src/server_streaming_call.c` のdeadline cleanup
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`

## Reviewer Role

- HTTP/2 / gRPC deadline・RST_STREAM・persistent connection lifecycle・current-state documentation adversary

## Review Prompt Summary

- 第九パスLowについて、current-state fallback clauseがRST submit即時失敗、`nghttp2_session_send` / socket / TLS / coalesced-buffer flushの全terminal failure、preflight drain capを実装どおり網羅するか再確認する。
- stream-local deadlineとconnection-scoped failure、deadとdraining、historical Planと現在の実装、fresh connection fallbackと同一call retryを混同していないかを監査する。
- 元issueのdeadline / RST_STREAM / persistent connection lifecycleだけを対象とし、style、一般的CI、無関係なdocsへscopeを広げない。

## Issues

none

## Prior Finding Recheck

### REVIEW-20260714-001: best-effort reuseのfallback説明がRSTの即時失敗を含まず狭い

- Status: `Fixed`（adequate）
- Fix commit: `f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- Verification: current-state paragraphはfallback集合を「RST submit / flushの失敗（grace deadline超過、submit即時失敗、`nghttp2_session_send` / socket / TLS / coalesced-buffer flushの任意のterminal failureを含む）またはpreflight drain cap超過」と明記した。`cancel_grpc_call_stream()`はRST submitの非0 returnをdeadへ遷移させ、`send_pending_h2_frames_with_deadline()`はsendまたはbuffer flushの非0 returnをdeadへ遷移させる。preflight drain cap超過はdeadではなくdrainingへ遷移し、cache adoptionを拒否する。文書はこれらを共通して「後続callがfresh connectionを使う」reuse fallbackとしてまとめる一方、state自体を同一視していない。

## Domain Gate Checks

- Stream / connection scope: read poll timeoutはgRPC Call / HTTP/2 Stream scopeの`RST_STREAM(CANCEL)`であり、RSTを安全にsubmit / flushできない場合だけHTTP/2 Connection scopeのterminal failureへ昇格する。
- Submit failure: `nghttp2_submit_rst_stream()`の即時失敗はconnectionをdeadにし、nghttp2 sessionを再駆動しない。PHPT 038はunary / server streamingのdeadline statusを維持しつつ、後続callがfresh connectionを使うことを固定する。
- Flush failure: `nghttp2_session_send`、socket / TLS write、coalesced-buffer flushのterminal failureは`send_pending_h2_frames_with_deadline()`でdeadとなる。retryableなWANT / EAGAINを「failure」と数える記述ではなく、helperからfailureとして返る経路の説明である。
- Drain cap: preflight drain cap超過は`mark_connection_draining()`で新規stream adoptionを止め、現在のfollow-up callをfresh connectionへfallbackさせる。deadとは記述していない。
- Fresh connection semantics: paragraphはpersistent connectionの「reuseはbest-effort」という文脈にあり、直前のcurrent-state説明も後続callでの再利用を対象にする。期限切れcall自身をfresh connectionでretryするという主張ではない。
- Historical / current boundary: Planは「当初計画」「superseded」と明示し、廃止したpost-RST drainを取り消し線で保持しつつ、現在採用されたsubmit / flush failure fallbackを説明する。current-state paragraph、SPEC §4.2、Decision Logと矛盾しない。
- Failure wording: 「任意の失敗」は実装がfailure returnとして扱う全branchを指し、成功・retryable I/Oまでconnection discardすると過大主張していない。即時failure後のdead化を安全側lifecycle contractとする説明も既存のfatal nghttp2 / partial-wire-state非再駆動contractに沿う。

## Scope Triage

- RST failureとpreflight capを同じ「fresh connection fallback」と呼ぶことは、次回adoption結果を表すまとめであり、dead / draining stateを同一視しないためfindingにしない。
- `fresh connection`の対象は周辺文脈上follow-up callであり、transparent retryを記述していないため追加文言は要求しない。
- current issueのDecision Logが50ms graceの設計理由を個別に説明することは、current-state paragraphの全failure集合と競合しない。

## Verification

- `git diff --check 76d282766091de75bd670f00cd05df8ef264335f..f94d3ba4dd95a8496b1d32efb372079b1ac89242`: PASS
- changed issueのBackground / Goals / Non-Goals / Plan / Progress / Decision Logを通読
- `docs/SPEC.md` §4.2とcurrent-state fallback clauseを相互確認
- `cancel_grpc_call_stream()`、`send_pending_h2_frames_with_deadline()`、preflight drain / cache adoptionの全関連failure branchをstatic review
- PHPT 033 / 035 / 038のsame-connection reuse、draining cap fallback、fatal submit後fresh connection oracleを確認
- docs-only差分のためDocker testはこのdomain subreviewでは未実行

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

