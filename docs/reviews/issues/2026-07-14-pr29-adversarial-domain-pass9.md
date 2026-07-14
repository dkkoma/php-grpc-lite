# PR #29 第九パス HTTP/2 / gRPC domain model review 2026-07-14

## Scope

- `1faf80a150dedbbf278a6cb4fbbf914ebb687b69..76d282766091de75bd670f00cd05df8ef264335f`
- 対応commit `1c55f56793bcf788b3d839942a3bee7b13487f43`
- レビュー記録commit `76d282766091de75bd670f00cd05df8ef264335f`
- `src/transport.c`（`destroy_h2_connection()` / trace lifecycle event）
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `docs/guides/code-reading-guide.md`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/SPEC.md` §4.2
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`

## Reviewer Role

- HTTP/2 / gRPC domain model・connection lifecycle・trace vocabulary・current-state documentation adversary

## Review Prompt Summary

- 第八パスのLow 2件について、local `h2_connection` destructorのtrace語彙・発火phase・test oracle、およびdeadline / RST_STREAM / persistent reuseの文書モデルが十分に修正されたか再確認する。
- 元issueのstream-scoped cancellationとbest-effort connection reuseを境界とし、一般的なtrace polishや無関係なCI hardeningへscopeを広げない。
- 対応差分がconnection / stream / call / channel scope、dead / draining lifecycle、production / diagnostic boundaryを変更していないことを確認する。

## Issues

none

## Prior Finding Recheck

### REVIEW-20260713-007: `wire.connection_close`とlocal destructor semanticsの不一致

- Status: `Fixed`（adequate）
- Fix commit: `1c55f56`
- Verification: `src/transport.c`のeventは`transport.connection_destroy`へrenameされ、`destroy_h2_connection()`入口、TLS/fd/nghttp2 session解放前に発火するlocal lifecycle eventであることをコメントへ明記した。`docs/guides/code-reading-guide.md`も同じ発火phase、preface前setup failureでも発火すること、`wire.connection_preface`と1対1ではないことを記載している。PHPT 041は新event名を数え、132件のdestructor invocation oracleを維持している。wire close、peer close、GOAWAY、TCP FIN/RSTとの混同は解消された。

### REVIEW-20260713-008: 元issueに残ったstreaming deadlineの現在形旧説明

- Status: `Fixed`（adequate）
- Fix commit: `1c55f56`
- Verification: 対象paragraphは「変更前」と明示され過去形になった。現在のunary / server streamingはいずれもdeadlineをstream-scoped `RST_STREAM(CANCEL)`として扱い、reuseはbest-effortで、RST flush grace deadline超過またはpreflight drain cap超過時にはfresh connectionへfallbackするという説明へ改められた。`docs/SPEC.md` §4.2、同issueのProgress / Plan / Decision Logと矛盾しない。

## Domain Gate Checks

- Naming: `transport.connection_destroy`はlocal HTTP/2 connection objectのdestructor invocationを表し、`wire.*` namespaceから分離されている。
- Lifecycle: event追加・renameは`destroy_h2_connection()`のTLS / fd / nghttp2 session teardown順、dead / draining遷移、cache detach、stream owner解放を変更していない。
- Scope: deadlineはgRPC Call / HTTP/2 Stream scope、socket/TLS/nghttp2 fatalとflush failureはHTTP/2 Connection scopeという既存taxonomyを維持する。
- Reuse: 通常のread timeoutはRST_STREAM(CANCEL)後にpersistent connectionを温存するが、RST flush failureおよびpreflight drain cap超過では安全側にfresh connectionへfallbackするbest-effort modelがSPECとissueで一致する。
- Production / diagnostic boundary: eventは既存のopt-in `GRPC_LITE_TRACE_FILE`経路だけに追加され、public PHP API、fault seam、bench surfaceへ新しい責務を露出しない。
- Test oracle: PHPT 041のevent filterとassertionはdestroy invocationを数えており、detach-only regressionを検出する目的を維持する。

## Scope Triage

- PHPT 041のlocal変数名`$closeCount`はevent filter・assertion・コメントがいずれもdestroyを明示しており、公開trace語彙やdomain invariantを誤らせない。renameはgeneric polishなのでfindingにしない。
- 過去のレビュー記録に`wire.connection_close`がhistorical finding / fix summaryとして残ることは、review historyを保持する運用に沿うためcurrent-state documentation defectではない。
- stable connection idはprefaceとのpairをcontractにしていないため不要。今回の元issuescopeへ追加しない。

## Verification

- `git diff --check 1faf80a150dedbbf278a6cb4fbbf914ebb687b69..76d282766091de75bd670f00cd05df8ef264335f`: PASS
- `wire.connection_close`のcurrent source / tests / guides / open issue / SPEC残存検索: none
- `transport.connection_destroy`の実装・PHPT・code-reading guide参照を相互確認
- issueのBackground / Goals / Plan / Progress / Decision Logと`docs/SPEC.md` §4.2を相互確認
- 対応差分にgRPC/HTTP/2 runtime behavior変更がないことをstatic reviewで確認
- Docker testはこのdomain-only subreviewでは未実行（親レビューのverification laneに委譲）

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

