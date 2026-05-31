# お手本化: grpc_call exchange state map と分割判断

- Status: Closed
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

`src/grpc_exchange_state.h` の `grpc_call` は、1 RPC over 1 HTTP/2 stream の交換状態を表している。現在は stream identity、status/validation flags、metadata、response parser、request writer、deadline/I/O error、bench-only observationを1つの大きなstructに持つ。

これは実装上は分かりやすい単一状態でもあるが、教材としては「どのfieldがどの責務に属するか」が見えづらい。またfield配置やsub-struct化はhot pathのcache localityに影響し得る。

## Goals

- `grpc_call` field mapを作り、各fieldの責務、lifetime、owner、hot/cold性を明確にする。
- sub-struct化する場合の候補とリスクを記録する。
- 実装する場合はbefore/after計測とdomain model reviewを必須にする。

## Non-Goals

- 計測なしのfield分割。
- status/metadata/deadline semanticsの変更。
- response parser side effect分離。
- performance改善を目的にした未計測の配置変更。

## Plan

1. `grpc_call` のfieldを次の分類で表にする。
   - stream lifecycle
   - status / validation flags
   - response metadata
   - response parser
   - request writer
   - deadline / I/O error
   - bench-only observation
2. 各分類についてhot path access頻度とlifetimeを整理する。
3. sub-struct化するか、現状維持 + docsで十分かを判断する。
4. 実装する場合は別phaseとしてbefore計測、変更、after計測、domain model reviewを行う。

## Performance Notes

このissueは性能影響が高い可能性がある。`grpc_call` は DATA chunk処理、nghttp2 callback、request write、status resolution、server streaming queueで頻繁に触られる。

実装前に必須:

- 仮説
- 対象ワークロード
- before計測
- 採否基準

計測候補:

- `./bench/run.sh spanner-shape`
- `./bench/run.sh metadata-header`
- unary 100B / 100KiB
- server streaming 100x100B / large streaming

## Progress

- 2026-05-31: 親issueから `grpc_call` exchange state作業を子issue化。
- 2026-05-31: `docs/design/grpc-call-exchange-state.md` を追加し、`grpc_call` fieldを責務、lifetime、hotnessで分類。
- 2026-05-31: `docs/guides/code-reading-guide.md` からfield mapへリンク。
- 2026-05-31: sub-struct化は実装しない判断を記録。実装する場合はbefore/after benchmarkとdomain model review必須。

## Verification

field mapだけなら:

- `git diff --check`: PASS
- ドキュメント変更のみ。C実装・field配置は未変更のため、C unit / PHPT / benchmarkは未実行。

実装する場合:

- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-coverage.sh`
- PHPUnit integration
- before/after benchmark
- HTTP/2/gRPC domain model review

## Decision Log

- 2026-05-31: `grpc_call` 分割はお手本化に有効だが、性能影響があり得るためdoc mapを先に作る。
- 2026-05-31: 現時点では `grpc_call` を分割しない。single structを維持し、field ownership mapを教材・レビュー補助として使う。
- 2026-05-31: field order / hot-cold layoutの評価は `docs/issues/open/2026-05-31-exemplar-grpc-call-field-layout-hotpath.md` へ切り出す。

## Close Criteria

- `grpc_call` field mapが記録されている。
- sub-struct化の採否が判断ログに残っている。
- 実装した場合はbefore/after計測とdomain model reviewが記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
