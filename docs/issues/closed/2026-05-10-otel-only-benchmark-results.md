# ベンチ結果保存をOTEL-onlyへ移行する

- Status: Closed
- Created: 2026-05-10
- Closed: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

Phase 2 runnerはOTEL span exportに対応したが、JSON/TSV互換出力、ResultContract、preset validation、単発の旧diagnostic suiteが残っていた。今後の比較は1世代前との再実行比較とOTEL summaryを使うため、旧baselineやJSON/TSV保存を維持する理由がなくなった。

## Scope

- `bench/phase2/run.sh` と代表compare scriptをOTEL span export + `otelop-summary.php` 集計に統一する。
- ベンチrunnerから `--output` / JSONファイル / TSV summary生成を外す。
- `ResultContract`、JSON schema、contract/cpu smoke、payload breakdown、metadata compat observationなどJSON保存前提の旧計測入口を削除する。
- libcurl時代のcurl trace診断optionを削除する。
- AGENTSとBenchmark GuideをOTEL-only運用へ更新する。

## Non-Goals

- 過去の研究docsや当時の測定結果を書き換えること。
- QA用のValgrind logやFPM lifecycle requestの内部JSON responseをOTELへ置き換えること。
- ライブラリ本体へOTEL instrumentationを再導入すること。

## Verification

- `docker compose run --rm dev sh -lc 'for f in tools/phase2/*.php; do php -l "$f" || exit 1; done'`
- `bash -n bench/phase2/run.sh bench/phase2/compare.sh bench/phase2/compare-spanner-dml-unary-shape.sh bench/phase2/compare-small-select-streaming.sh tools/test/check-native-lifecycle-stress.sh tools/test/check-native-slow-consumer.sh`
- `BENCH_TAG=otel-only-final2-20260510-162723 ./bench/phase2/run.sh throughput-unary --duration=0.02 --warmup-calls=1 --payload-bytes=10`
- `BENCH_TAG=otel-only-compare-final-20260510-162733 ./bench/phase2/compare.sh throughput-unary --duration=0.01 --warmup-calls=1 --payload-bytes=10`
- `BENCH_TAG=otel-only-spanner-smoke-20260510-162511 DURATION=0.01 WARMUP_CALLS=1 MAX_CALLS=2 ./bench/phase2/compare-spanner-dml-unary-shape.sh`
- OTEL summary confirmed php-grpc-lite / ext-grpc spans from otelop without JSON/TSV result files.
