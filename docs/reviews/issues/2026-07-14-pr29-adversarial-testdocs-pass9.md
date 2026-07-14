# PR #29 第九パス敵対的レビュー（test oracle / build boundary / issue documentation） 2026-07-14

## Scope

- PR #29 `1faf80a150dedbbf278a6cb4fbbf914ebb687b69..76d282766091de75bd670f00cd05df8ef264335f`
- 対応commit `1c55f56793bcf788b3d839942a3bee7b13487f43`
- 記録commit `76d282766091de75bd670f00cd05df8ef264335f`
- `src/transport.c` のtrace event renameとlocal connection lifecycle semantics
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt` のdestroy oracle
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/guides/code-reading-guide.md`、`docs/SPEC.md`、`docs/verification/protocol-model-review-guide.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md` のREVIEW-20260713-007〜008

## Reviewer Role

- PR adversary / test-oracle・build-boundary・issue-documentation reviewer

## Review Prompt Summary

- 第八パスLow 2件がtrace vocabulary、test oracle、元issueのcurrent/historical model、review recordの各境界で完全に修正されたかを静的に再レビューする。
- deadline / RST_STREAM / persistent connection reuse lifecycleという元issueのscope内に限定し、一般的なCI matrixやbench hardeningは指摘対象から除外する。

## Issues

none

## Prior Low Recheck

- REVIEW-20260713-007はadequate。eventは`wire.connection_close`から`transport.connection_destroy`へ一貫してrenameされ、`destroy_h2_connection()`入口で発火するlocal lifecycle eventであること、TLS/fd/nghttp2 session解放前であること、preface前のsetup failureでも発火し得るため`wire.connection_preface`と1対1ではないことが実装コメントとcode-reading guideの双方に明記された。旧event名はhistorical review recordにだけ残り、現行実装・現行guide・PHPT oracleには残っていない。
- PHPT 041のoracleはrename後もadequate。132回のprefaceと132回の`transport.connection_destroy`を独立にcountし、fatal mem-recv経路でconnectionがcacheからdetachされるだけでなくdestructorへ到達したことを固定する。変更はevent selectorだけで、既に実証されたdestroy call除去mutationへの検出構造を弱めていない。`wire.frame_out`のRST_STREAM非出現assertionとcache limit sweepも維持されている。
- REVIEW-20260713-008はadequate。streaming deadlineの旧挙動は「変更前」かつ過去形へ修正され、現在のunary / server streamingは共通のstream-scoped RST_STREAM(CANCEL)、connection reuseはbest-effortと明記された。Background、Goals / Non-Goals、superseded Plan、Progress、Decision Log、Verification、Close CriteriaはSPEC §4.2と整合する。
- REVIEW-20260713-007〜008のcentral review recordは、過渡的な旧event名と修正理由をreview historyとして保持し、現行/future modelはSPEC・code-reading guide・issue本文へ置く境界を守っている。Status、Fix summary、Fix commit、Verification、件数集計も差分と一致する。
- 対応差分はconfig/build seamを変更していない。既存の`--enable-grpc-bench` / `--enable-grpc-test-fault`境界やrunner期待値への新たな影響はないため、一般的なbuild matrix拡張は元issue外として扱った。

## Static Verification

- `git diff 1faf80a..76d2827` と変更5ファイルを全件確認。
- `wire.connection_close` / `transport.connection_destroy` のrepository-wide参照を照合し、旧名は過去のreview historyだけに残ることを確認。
- `destroy_h2_connection()`、trace open/lock helper、preface前setup failure call site、PHPT 029 / 041を照合。
- 元issueのBackground / Goals / Non-Goals / Plan / Progress / Verification / Decision Log / Close CriteriaをSPEC §4.2と相互確認。
- `git diff --check 1faf80a..76d2827`: pass。
- 動的テストは親エージェントが実施するため、本記録では静的レビューのみ。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
