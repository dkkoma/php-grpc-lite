# PR #29 第十一パス HTTP/2 / gRPC domain model review 2026-07-14

## Scope

- `f94d3ba4dd95a8496b1d32efb372079b1ac89242..b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`
- `docs/SPEC.md` §4.2
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `poc/test-server/main.go` の `serveBacklogFloodH2C()` fixture
- `src/transport.c` の `preflight_persistent_connection()` / `drain_pending_connection_data_for_reuse()` と dead / draining lifecycle

## Reviewer Role

- HTTP/2 / gRPC deadline・RST_STREAM・persistent connection lifecycle・test-effective / production-default contract adversary

## Review Prompt Summary

- 第十パスLowについて、PHPT 035のコメントがproduction default 64KiBとtestで設定したeffective cap 16KiBを区別し、fixtureとassertionが実際に証明する範囲だけを述べるか再確認する。
- stream-local cancelとconnection adoption、deadとdraining、TCP receive bufferとHTTP/2 flow-control window、current-state test contractとhistorical review recordを混同していないかを監査する。
- 元issueのdeadline / RST_STREAM / persistent connection reuse scope内で、Blocker / High / Medium / Low / Design Decisionをすべて評価する。

## Issues

none

## Prior Finding Recheck

### REVIEW-20260714-002: PHPT 035のコメントが検証していないproduction 64KiB capを主張

- Status: `Fixed`（adequate）
- Fix commit: `b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- Verification: 修正後コメントは、fixtureの48KiB backlogが `--INI--` のconfigured 16KiB capを超えるためpreflight adoptionを断念すること、production default 64KiB境界そのものはこのtestで跨いでいないこと、したがってtest oracleはdefault値ではなく縮小したeffective capでcap機構を固定することを明記した。冒頭fixture説明、`$preflightBytes === 16384`相当の上下限assertion、SPEC §4.2のproduction default 64KiBと矛盾しない。

## Domain Gate Checks

- Stream / connection scope: `$call->cancel()` は対象HTTP/2 streamをRST_STREAMで閉じる。その後のfollow-up callによるpersistent connection adoption時に、ownerのいないconnectionへ残ったbytesをpreflight drainする。stream cancel自体と次回connection adoptionを同一操作として説明していない。
- Dead / draining: preflight drainが16KiBのread budgetを使い切りread boundaryへ到達しなかった場合、runtimeはconnectionをdeadではなく`draining`にして新規streamをadmitせず、follow-up callをfresh connectionへfallbackする。コメントの「gives up (draining)」はこのstate transitionと一致する。
- Cap semantics: runtimeは各readを残budget以下へ制限し、設定済み16384 bytesを超えて読まない。fixtureは3 x 16KiB DATA frameの48KiB backlogを用意するため、cap到達後にも未読bytesが残る前提とtest assertionが一致する。
- Production / test boundary: `GRPC_LITE_PREFLIGHT_DRAIN_DEFAULT_MAX_BYTES`とSPEC §4.2のdefaultは65536 bytesだが、PHPT 035は`grpc_lite.preflight_drain_max_bytes=16384`を明示する。修正文はdefault値を検証済みとせず、effective設定値で同じlifecycle mechanismを固定するtestとして説明する。
- Buffer / flow-control boundary: fixtureの制約はclient kernelのTCP receive bufferへ未読backlogを滞留させるためのもの。HTTP/2 stream / connection receive windowの設定とpreflight read budgetを同一概念として主張していない。
- Historical / current boundary: `REVIEW-20260714-002`は過渡的なfindingと対応履歴として旧誤記を保持し、`Status: Fixed`、Fix summary、Verification、総Low件数、第十パス履歴を追加した。current-state test commentだけが修正済みの契約を表し、履歴と現在仕様の境界は保たれる。
- Follow-up semantics: testは期限切れ／cancel済みcall自身をretryせず、別の後続unary callがfresh connectionで成功することを固定する。best-effort reuse fallbackとtransparent retryを混同していない。

## Scope Triage

- production default 64KiB境界を直接跨ぐ追加fixtureは、kernel receive-window制約を回避する別設計を要する一方、今回のLowはtest commentの証拠範囲訂正だけを要求していた。16KiBへ下げたeffective capでruntimeのbounded-drain / draining fallbackを固定できているため、新規testは要求しない。
- `Fix commit: 記録反映と同一コミット`は、test comment修正とreview record更新を同一commitに含める自己参照不能な履歴境界の記法であり、findingにしない。
- production codeは今回のresponse commitで変更されておらず、既存lifecycleを再設計する指摘や一般的なCI拡張は元issueのscope外として採用しない。

## Verification

- `git diff --check f94d3ba4dd95a8496b1d32efb372079b1ac89242..b7b9fd13b177d2426cb07c28f1f12717cc670e6d`: PASS
- `git diff --check main...b7b9fd13b177d2426cb07c28f1f12717cc670e6d`: PASS
- PHPT 035の`--INI--`、48KiB fixture barrier、16384-byte preflight trace assertion、fresh-connection assertionを静的に相互確認
- `drain_pending_connection_data_for_reuse()`のread上限、cap超過時`mark_connection_draining()`、cache adoption拒否を静的に確認
- SPEC §4.2、open issue、central review recordをcurrent / historical boundaryの観点で通読
- response commitはcomment / review recordのみのため、このdomain subreviewではDocker testを重複実行していない

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
