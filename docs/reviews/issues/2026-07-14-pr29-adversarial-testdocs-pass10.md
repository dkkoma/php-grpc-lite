# PR #29 第十パス敵対的レビュー（test oracle / issue documentation / review record） 2026-07-14

## Scope

- PR #29 `76d282766091de75bd670f00cd05df8ef264335f..f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`
- `docs/SPEC.md` §4.2
- `src/transport.c` のcancel / send failure lifecycle
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`

## Reviewer Role

- PR adversary / test-oracle・issue-documentation・review-record reviewer

## Review Prompt Summary

- 第九パスLowの対応がRST submit即時失敗、flush失敗全般、grace deadline超過、preflight drain capのfallback集合をSPEC・実装・テストと一致させたかを静的に再レビューする。
- 元issueのBackground / 公式比較 / Goals / Non-Goals / Plan / Progress / Decision Log / Verification / Close Criteriaとcentral review recordを確認し、deadline / RST_STREAM / connection reuse scope内のactionableな矛盾だけを指摘する。

## Issues

### REVIEW-20260714-002: PHPT 035が設定済み16KiB capを64KiB capと説明している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `test-oracle / issue-documentation adversary`
- Finding: PHPT 035は`grpc_lite.preflight_drain_max_bytes=16384`を指定し、fixtureがclient kernelへ滞留させる48KiB backlogがこの**設定済み16KiB cap**を超える形でfresh-connection fallbackを固定している。しかしfollow-up直前のコメントは「more backlog than the 64KiB drain cap」と記載し、同じtestのINI・fixture説明・assertionおよびSPECのproduction default 64KiBと矛盾する。このtestの48KiB backlogはproduction default 64KiBを超えていない。
- Evidence: `tests/phpt/035-preflight-drain-cap-fallback.phpt:14`（cap 16384）、同`:31-39`（48KiB backlogとproduction capを下げる理由）、同`:60-63`（誤った64KiB表記）、同`:95-96`（16384 bytesのcap assertion）、`docs/SPEC.md:92`（default 64KiB）。
- Expected model: test oracleの説明は、production defaultとtestで縮小したeffective capを区別する。PHPT 035が証明するのは「48KiB backlog > configured 16KiB cap」であり、「backlog > production 64KiB cap」ではない。
- Why it matters: 現コメントはPHPT 035がproduction defaultの64KiB境界を直接通過させたと誤読させ、kernel receive-window制約を理由にcapを縮小したtest設計と反対の証拠範囲を主張する。将来fixture backlogやINIを変更する際のoracle維持判断も誤らせるため、単なる語調ではなくtest contractの事実誤認である。
- Recommended fix: 当該コメントを「with more backlog than the configured 16KiB drain cap」等へ修正し、必要なら同じコメント内でproduction defaultは64KiBだがkernel window制約のためtestでは16KiBへ縮小していると明記する。test behaviorや追加CIは不要。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `--INI--`、48KiB fixture barrier、`$preflightBytes`の16384上下限assertion、SPEC §4.2、issue Decision LogのPHPT 035説明を静的に相互照合。
- Notes: 第九パスdocs差分から生じた問題ではないが、今回明示されたPHPT 035 oracle reviewかつ元issueのbest-effort reuse境界そのものであるためscope内。一般的なtest追加要求ではない。

## Adequate Fixes Confirmed

- 元issueのcurrent-state文は、RST submit即時失敗、`nghttp2_session_send` / socket / TLS / coalesced-buffer flush失敗、grace deadline超過、preflight drain cap超過をbest-effort reuseのfallback集合として列挙し、`cancel_grpc_call_stream()`、`send_pending_h2_frames_with_deadline()`、SPEC §4.2、PHPT 033 / 038と一致する。
- Planのsuperseded注記と旧drain項も「grace deadline超過だけ」から「RST submit / flush失敗全般」へ揃えられ、Background、Goals / Non-Goals、Progress、Decision Log、Verification、Close Criteriaとの新たな矛盾はない。
- REVIEW-20260714-001のStatus、Finding、Expected model、Fix summary、Verification、Low件数は差分と一致する。Fix commitの「記録反映と同一コミット」はdocs修正とrecordを同じ`f94d3ba`で行った自己参照不能なcommitに対する正当な記法で、history boundaryも保たれている。
- 既存review recordに残る過去の狭い文言や旧event名は当時のfinding / fix historyであり、現行仕様としての残存ではない。

## Scope Triage

- docs-only responseのため、runtime test追加、CI matrix、一般的なfault coverage拡張は要求しない。
- PHPT 035のbehavioral assertion自体はadequateで、指摘はeffective capを誤記するoracle commentの訂正だけに限定する。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none
