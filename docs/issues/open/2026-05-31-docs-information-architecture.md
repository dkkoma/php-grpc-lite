# docs情報設計: 入口、階層、リンク方針の整理

- Status: Open
- Created: 2026-05-31
- Branch: codex/docs-information-architecture-issue
- Owner: Codex
- Parent: none
- Related:
  - docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md
  - docs/issues/closed/2026-05-31-exemplar-docs-fixtures-and-reading-guide.md
  - docs/issues/closed/2026-05-31-exemplar-connection-ownership-model.md

## Background

お手本化作業の中で、`grpc_call` field map、connection / stream ownership、transport design、code reading guideなど、近い粒度のドキュメントが増えた。

内容としては必要でも、`docs/` 直下へ説明したい単位でファイルを追加すると、どれが入口で、どれが設計書で、どれが読み物で、どれが検証資料なのかが分かりにくい。今回の `connection-stream-ownership.md` 案も、既存の `docs/grpc-call-exchange-state.md` と `docs/http2-transport-design.md` に近い内容があり、独立docとして追加すると重複して見通しが悪くなることが分かった。

このissueでは、個別docの中身ではなく、docs全体の情報設計を扱う。

## Goals

- docs全体の入口を決め、初学者から上級者まで目的別に辿れるようにする。
- 主要な現行ドキュメントへ入口からリンクし、孤立したdocを減らす。
- `docs/` 直下にフラットに増える状態を見直し、必要なら階層を作る。
- design doc、reading guide、verification guide、benchmark record、review / issue履歴の置き場所と役割を明確にする。
- 新しいドキュメントを追加する前に、既存docへ統合すべきか判断できる方針を作る。

## Non-Goals

- C実装、PHP wrapper、test behaviorの変更。
- 既存設計判断の書き換え。
- historical issue / review / benchmark recordの内容整理。
- 全ドキュメントの全面リライト。

## Plan

1. `docs/` 直下と主要subdirectoryを棚卸しし、現行docを役割で分類する。
2. docs入口を決める。
   - 候補: `docs/README.md`
   - `README.md` からのリンク位置も確認する。
3. 入口から主要docへリンクする情報構造を作る。
   - project overview / spec
   - code reading
   - transport / protocol design
   - API compatibility
   - verification / fixtures / release QA
   - benchmark / instrumentation
   - reviews / issues
4. 階層化の候補を出す。
   - 例: `docs/design/`
   - 例: `docs/guides/`
   - 例: `docs/verification/`
   - 例: `docs/benchmarks/` は既存維持
   - 例: `docs/reviews/`、`docs/issues/` は既存維持
5. 移動する場合はリンク更新と `rg` による参照確認を行う。
6. 以後のドキュメント追加方針を短く明文化する。

## Performance Notes

docs構造とリンク整理のみならruntime performanceへの影響はない。

ファイル移動だけでも、テストrunner、CI、README、AGENTS、issue本文、review記録から参照されているpathが変わる可能性はある。実装コードには触れないが、リンク切れ確認は必須にする。

## Verification

- `rg` による旧path参照確認
- `git diff --check`
- docs-onlyならPHPT / PHPUnit / benchmarkは不要
- path移動を含む場合は、少なくとも関連scriptやREADME参照のsmoke確認を行う

## Decision Log

- 2026-05-31: `docs/connection-stream-ownership.md` 案は既存docと重複するため、独立docではなく `docs/grpc-call-exchange-state.md` へ統合した。
- 2026-05-31: 同種の重複を避けるため、docs全体の入口、階層、リンク方針を独立issueとして扱う。

## Close Criteria

- docs全体の入口が定義されている。
- 主要な現行ドキュメントが入口から辿れる。
- 新規doc追加時に、独立docにするか既存docへ統合するかの判断方針がある。
- 階層を作る場合は、リンク更新と旧path参照確認が完了している。
- 必要な検証結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
