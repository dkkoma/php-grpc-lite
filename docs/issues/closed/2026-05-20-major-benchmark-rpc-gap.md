---
Status: Closed
Owner: Codex
Created: 2026-05-20
Related:
  - docs/issues/open/2026-05-20-spanner-gap-pacing-investigation.md
---

# ベンチマークのRPC間gapを明示条件にする

## 目的

GitHub issue #5 のreal Spanner `SELECT 1` 調査で、gapなし連続RPCと10ms inter-RPC gapでresponse timingが大きく変わることがわかった。ただしgapが意味を持つのはreal Spanner側のpacing / scheduling調査であり、Go test-serverやemulator向けの通常ベンチに既定で入れる意味は薄い。ベンチrunnerではgapを明示条件として扱い、OTEL上で比較可能にする。

## 方針

- RPC spanの中にはsleepを含めない。
- sleepは1 RPC / 1 streamの測定spanを記録した後、次RPC開始前に入れる。
- OTEL attributesに `benchmark.rpc_gap_ms` を必ず出す。
- すべての通常suiteの既定gapは0msにする。
- real Spanner pacing調査では `--rpc-gap-ms` / `BENCH_RPC_GAP_MS` で10msを明示指定する。

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

- 通常suiteの既定gapが0msである。
- gap値がOTEL集計の属性で確認できる。
- 明示指定時のみgapを持つ。

## 検証

- `docker compose run --rm dev sh -lc 'php -l ...'` で変更したbenchmark PHPの構文チェックを実行。
- `BENCH_TAG=rpc-gap-smoke BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh spanner-shape --calls=2 --warmup-calls=1` を実行し、summaryの `gap_ms` が `10` になることを確認。ただしこの方針は2026-05-20見直しで撤回した。
- `BENCH_TAG=rpc-gap-throughput-smoke BENCH_RPC_GAP_MS=1 BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh throughput-unary --duration=0.1 --warmup-calls=1` を実行し、throughput系でも明示指定時に `gap_ms` が `1` になることを確認。
- `BENCH_TAG=rpc-gap-default0-smoke BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh spanner-shape --calls=2 --warmup-calls=1` を実行し、通常suiteのsummaryが `gap_ms=0` になることを確認。

## 2026-05-20 見直し

gapはreal Spanner側のpacing / scheduling差を観測するための条件であり、Go test-serverやemulatorに投げる通常benchmarkへ既定で入れる意味は薄い。`bench/run.sh` の既定gapは全suite 0msへ戻し、real Spanner調査では `BENCH_RPC_GAP_MS=10` を明示する運用にする。

## Fix summary

- `bench/run.sh` の既定 `BENCH_RPC_GAP_MS` を全suite 0msに戻した。
- real Spanner pacing調査は `BENCH_RPC_GAP_MS=10` / `--rpc-gap-ms=10` の明示指定で扱うことをdocsに記録した。
- ベンチPHP実行とOTEL summary実行は、コンテナ内実行前提として `memory_limit=-1` にした。

## Verification

- `bash -n bench/run.sh bench/compare.sh`
- `docker compose run --rm dev sh -lc 'php -l tools/benchmark/rtt-unary.php && php -l tools/benchmark/spanner-shape.php && php -l tools/benchmark/spanner-real-client.php'`
- `BENCH_TAG=rpc-gap-default0-smoke BENCH_OTEL_SUMMARY_LIMIT=1000 ./bench/run.sh spanner-shape --calls=2 --warmup-calls=1`
