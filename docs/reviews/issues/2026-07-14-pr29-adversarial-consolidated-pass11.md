# PR #29 第十一パス敵対的レビュー統合 2026-07-14

## Scope

- PR #29 前回レビュー済みHEAD `f94d3ba4dd95a8496b1d32efb372079b1ac89242`
- 第十パス対応commit / 現在HEAD `b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `poc/test-server/main.go` のbacklog flood fixture
- `src/transport.c` のpreflight drain、dead / draining、persistent cache adoption lifecycle
- `docs/SPEC.md` §4.2
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`

## Reviewer Role

- PR adversary / HTTP/2・gRPC domain model / C lifetime / test oracle・fixture・documentation統合 reviewer

## Review Prompt Summary

- 第十パスLowが完全に修正されたかを確認し、元issueのdeadline / RST_STREAM / connection lifecycle scope内でrequired gateを再実施した。
- domain model、C lifetime、test oracle・fixture・review recordを独立にレビューし、cross-triage後に全重要度の残指摘がないことを確認した。

## Issues

none

## Prior Finding Recheck

### REVIEW-20260714-002: PHPT 035のコメントが検証していないproduction 64KiB capを主張

- Status: `Fixed`（adequate）
- Fix commit: `b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- Verification: 修正後コメントは、48KiB backlogが`--INI--`で設定した16KiB capを超えるためpreflightがdrainingとなりfresh connectionへfallbackすること、production defaultの64KiB境界自体はこのtestで跨いでいないこと、したがって縮小したeffective capで同じ機構を固定するtestであることを明記した。INI、fixture、16384-byte上下限assertion、SPEC §4.2と一致する。

## Domain / Lifecycle Gate

- `$call->cancel()`のstream-local RSTと、owner解除後のfollow-up callによるconnection adoption preflightを混同していない。
- preflight cap到達はconnection failureの`dead`ではなく、新規stream admissionを禁止する`draining`であり、コメントの`gives up (draining)`と一致する。
- runtimeは各readを残budget以下へ制限し、このcaseでは累計16384 bytesでcapへ到達する。testは`>= 16384`と`<= 16384`を両方assertする。
- production default 65536 bytesとtest effective cap 16384 bytesを区別し、48KiB fixtureがdefault境界を直接検証したとは主張していない。
- fresh connectionは`persistent_reused=false`、preface 2件、follow-up `STATUS_OK`で固定される。期限切れcall自身のtransparent retryを意味しない。
- central review recordはREVIEW-20260714-002をFixedとして追記し、High 7 / Medium 9 / Low 15 / Design Decision 1、全32件Fixedの集計と第十パス履歴が一致する。

## Scope Triage

- open issueのVerificationへ第九・第十パスを重複転記していない点は、central review recordに対応・検証が残り、現在仕様やClose Criteriaを誤らせないためfindingにしない。
- production 64KiB境界を直接跨ぐ別fixture、一般的なCI追加、runtime再設計は今回のcomment-only対応および元issueの必要範囲外として要求しない。
- `Fix commit: 記録反映と同一コミット`は、fixとrecordを同一commitへ含める自己参照不能な履歴境界の記法として妥当である。

## Verification

- ASan / UBSan production lane: 22 PASS / 4 SKIP、reportなし
- ASan / UBSan bench+fault lane: 26/26 PASS、reportなし
- NTS PHPT: 26/26 PASS
- ZTS PHPT: 24 PASS / 2 SKIP、0 FAIL
- C unit: protocol_core / status_core / transport_core PASS
- C static analysis: production / benchともexit 0
- PHPUnit: 31 tests / 116 assertions PASS
- `bash -n`（PR変更shell script 4本）: PASS
- `git diff --check main...HEAD`、`git diff --check f94d3ba4dd95a8496b1d32efb372079b1ac89242..HEAD`: PASS
- GitHub review result: `https://github.com/dkkoma/php-grpc-lite/pull/29#issuecomment-4963671678`（再取得してHEAD / verdict / verification本文一致を確認）

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
