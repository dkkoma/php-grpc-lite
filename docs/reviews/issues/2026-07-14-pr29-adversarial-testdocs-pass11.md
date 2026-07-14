# PR #29 第十一パス敵対的レビュー（test oracle / fixture / issue-review record） 2026-07-14

## Scope

- PR #29 `f94d3ba4dd95a8496b1d32efb372079b1ac89242..b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `poc/test-server/main.go` の backlog flood fixture（`:50068` / control `:50069`）
- `src/transport.c` の `effective_preflight_drain_max_bytes()` / `drain_pending_connection_data_for_reuse()`
- `docs/SPEC.md` §4.2
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`

## Reviewer Role

- PR adversary / test-oracle・fixture・issue-review-record reviewer

## Review Prompt Summary

- 第十パスLowについて、PHPT 035の説明が`--INI--`で設定した16KiB cap、48KiB backlog fixture、cap到達assertion、production既定64KiBの非検証範囲を正確に区別するか再確認する。
- 中央review recordのREVIEW-20260714-002、severity / status集計、第十パスhistoryが差分と一致するか監査する。
- 元issueのdeadline / RST_STREAM / persistent reuse scope内に限定し、一般的なtest / CI拡張は要求しない。

## Issues

none

## Prior Finding Recheck

### REVIEW-20260714-002: PHPT 035のコメントが検証していないproduction 64KiB capを主張

- Status: `Fixed`（adequate）
- Fix commit: `b7b9fd13b177d2426cb07c28f1f12717cc670e6d`
- Verification: follow-up call直前の説明は、effective capを`--INI--`で設定した16KiBと明示し、fixtureのbacklogがこの設定値を超えるためpreflightがdrainingへ遷移してfresh connectionへfallbackするcontractを記述するようになった。さらにproduction既定64KiB境界はkernel receive-window制約から直接跨いでおらず、このtestが固定するのは縮小した設定値でのcap機構だと明記したため、旧コメントの証拠範囲の過大主張は解消した。

## Test Oracle / Fixture Checks

- Effective cap: PHPTの`--INI--`は`grpc_lite.preflight_drain_max_bytes=16384`であり、`effective_preflight_drain_max_bytes()`の下限4096より大きいため実効値は正確に16KiBとなる。
- Backlog barrier: fixtureはtiny `SO_SNDBUF`で16KiB DATAを3回、計48KiB送信し、Write完了後にcontrol channelへ`ready`を返す。PHPTは`ready`受領後にcancelするため、設定済み16KiB capを上回るbacklogをclient側へ到着させる既存barrier contractと整合する。
- Cap assertion: traceのpositive `wire.socket_preflight_read.result_len`合計について`>= 16384`と`<= 16384`を両方assertし、cap到達とread上限を同時に固定する。fresh fallbackは`persistent_reused=false`、connection preface 2本、follow-up `STATUS_OK`で固定される。
- Production boundary: `GRPC_LITE_PREFLIGHT_DRAIN_DEFAULT_MAX_BYTES`、PHPT 002、SPEC §4.2はいずれもdefault 65536 / 64KiBで一致する。PHPT 035の48KiB fixtureはdefault境界を跨いだとは主張せず、縮小したeffective capで同じfallback機構を検証すると正しく限定している。
- Wording / behavior: コメントの「configured 16KiB cap」「production 64KiB boundary itself is never crossed」「pins the cap mechanism at the lowered setting」は、INI、fixture、assertion、SPEC、元issue Decision Logのすべてと矛盾しない。

## Review Record Checks

- REVIEW-20260714-002はSeverity `Low`、Status `Fixed`、Finding / Evidence / Expected model / Recommended fix / Fix summary / Verificationを備え、今回の差分と一致する。
- 中央recordを再計数し、High 7、Medium 9、Low 15、Design Decision 1、合計32件がすべてFixedであることを確認した。Review Resultの件数更新は正しい。
- 第十パスhistoryはreview対象HEAD `f94d3ba`とLow 1件追加・Fixedを正しく記録する。fixとrecordが同一commitに入るため、Fix commitの「記録反映と同一コミット」も自己参照不能な履歴として矛盾しない。

## Scope Triage

- 今回はtest commentとreview historyだけの変更であり、runtime behavior、fixture動作、INI値、assertionは変更されていない。追加test / CI matrixは要求しない。
- production 64KiBを直接跨ぐfixtureを新設する要求は、既知のkernel-window制約があり、今回のコメント修正にも不要なためfindingにしない。

## Verification

- `git diff --check f94d3ba4dd95a8496b1d32efb372079b1ac89242..b7b9fd13b177d2426cb07c28f1f12717cc670e6d`: PASS
- PHPT 035、backlog fixture、preflight drain実装、SPEC §4.2、元issue、中央review recordを静的に相互照合
- 中央review recordのseverity / statusを機械的に再計数（High 7 / Medium 9 / Low 15 / Design Decision 1、Fixed 32）
- comment / review-recordのみの差分であり、このsubreviewではDocker testを重複実行していない

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
