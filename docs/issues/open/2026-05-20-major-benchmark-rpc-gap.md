---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - docs/issues/open/2026-05-20-spanner-gap-pacing-investigation.md
---

# 主要ベンチマークにRPC間gapを入れる

## 目的

GitHub issue #5 のSpanner `SELECT 1` 調査で、gapなし連続RPCと10ms inter-RPC gapでresponse timingが大きく変わることがわかった。主要ベンチマークでもこの条件を明示的に扱い、grpc-lite / ext-grpc比較が現実的なRPC pacingを含む形になるようにする。

## 方針

- RPC spanの中にはsleepを含めない。
- sleepは1 RPC / 1 streamの測定spanを記録した後、次RPC開始前に入れる。
- OTEL attributesに `benchmark.rpc_gap_ms` を必ず出す。
- saturation throughput系は目的が最大処理量なので、既定gapは0msのままにする。ただし `--rpc-gap-ms` / `BENCH_RPC_GAP_MS` で明示指定できるようにする。
- Spanner / RTT系の主要比較線では既定gapを10msにする。

## スコープ

- `bench/run.sh` のsuite別既定値。
- `tools/benchmark/*` の主要suiteに `--rpc-gap-ms` を追加。
- `docs/benchmarks/README.md` に測定条件を明記。

## 非スコープ

- 既存の過去ベンチ結果の再計測。
- production transportのpacing変更。
- real Spanner診断CLIの修正。

## 進捗

- [x] 実装
- [x] 代表suiteのsmoke実行
- [x] docs更新

## 完了条件

- 主要Spanner / RTT suiteが既定で10ms inter-RPC gapを持つ。
- gap値がOTEL集計の属性で確認できる。
- throughput系は明示指定時のみgapを持つ。

## 検証

- `docker compose run --rm dev sh -lc 'php -l ...'` で変更したbenchmark PHPの構文チェックを実行。
- `BENCH_TAG=rpc-gap-smoke BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh spanner-shape --calls=2 --warmup-calls=1` を実行し、summaryの `gap_ms` が `10` になることを確認。
- `BENCH_TAG=rpc-gap-throughput-smoke BENCH_RPC_GAP_MS=1 BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh throughput-unary --duration=0.1 --warmup-calls=1` を実行し、throughput系でも明示指定時に `gap_ms` が `1` になることを確認。
