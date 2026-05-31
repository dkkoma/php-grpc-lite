# OTELベンチ計測をPHP runner境界だけに限定する

- Status: Closed
- Created: 2026-05-10
- Closed: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

C拡張内部telemetry recordをactive spanへ付与する設計は、`random_bytes()`、attribute構築、callback処理がhot pathに入り、DML unaryのp99をmainのJSON/TSV計測より大きく歪めた。

ベンチ比較では、php-grpc-liteとext-grpcを同じPHP runner境界で測ることが重要。ライブラリ内部観測は捨て、PHP側で旧JSON/TSVと同じ `hrtime()` 境界をOTEL spanとして後組み立てする。

## Goals

- C拡張内部telemetry callbackをbench計測から外す。
- ベンチrunnerのPHP側で `startNs` / `endNs` だけを測り、RPC終了後にspan recordを保存する。
- OTLP exportは測定終了後batchで行う。
- ext-grpcとphp-grpc-liteを同じ境界でOTEL集計できるようにする。
- mainのJSON/TSV結果に近いDML p99へ戻るか確認する。

## Non-Goals

- ライブラリ利用時の内部観測を有効化すること。
- `grpc_lite.*` 内部timing属性をspanへ付与すること。
- C拡張のtelemetry ini/APIをmainへ取り込むこと。

## Changes

- `BenchTelemetry` はC拡張handlerを登録せず、PHP runner境界の `startNs` / `endNs` からOTLP spanを後生成する形にした。
- Phase 2 runnerのtelemetry wrapperを廃止し、既存JSON/TSVと同じRPC前後の `hrtime()` 境界で `recordRpcSpan()` を呼ぶ形にした。
- C拡張runtime telemetry API、telemetry INI、PHP telemetry adapterを削除した。
- C拡張側に残すのはbench build限定のdiagnostic record helperだけにした。
- `otelop-summary.php` はspan durationだけを表示し、内部duration列を削除した。
- `docs/guides/opentelemetry-instrumentation.md` はtrace context metadata注入とbenchmark OTEL exportのみを説明する最終形に直した。

## Verification

- `docker compose run --rm dev sh -lc 'for f in tools/benchmark/*.php; do php -l "$f" || exit 1; done'`
- `./tools/test/check-phpt.sh`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `BENCH_TAG=otel-php-boundary-dml-20260510-140115 BENCH_OTEL_RUN_ID=otel-php-boundary-dml-20260510-140115 BENCH_OTEL_EXPORTER=otlp-http BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 BENCH_OTEL_SUMMARY_LIMIT=20000 ./bench/compare-spanner-dml-unary-shape.sh`
- `BENCH_TAG=otel-php-boundary-small-select-20260510-140059 BENCH_OTEL_RUN_ID=otel-php-boundary-small-select-20260510-140059 BENCH_OTEL_EXPORTER=otlp-http BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces WARMUP_STREAMS=10 INCLUDE_FRANKEN=0 INCLUDE_POC=0 BENCH_OTEL_SUMMARY_LIMIT=20000 ./bench/compare-small-select-streaming.sh`

## Results

### Spanner DML unary shape

| case | php-grpc-lite p50/p99 | ext-grpc p50/p99 |
|---|---:|---:|
| begin_txn | 28.9 / 185.3 μs | 63.8 / 236.7 μs |
| dml_insert_10col | 34.9 / 186.2 μs | 63.6 / 381.9 μs |
| dml_update_10col | 26.0 / 67.1 μs | 55.3 / 249.8 μs |
| dml_delete_10col | 24.4 / 63.9 μs | 66.4 / 106.0 μs |
| commit_txn | 24.9 / 91.7 μs | 65.3 / 191.4 μs |

- TSV: `var/bench-results/phase2-spanner-dml-unary-shape-otel-php-boundary-dml-20260510-140115.tsv`
- OTEL summaryはTSVと同じp50/p99を返した。
- C内部telemetry付与時に見えていたDML p99悪化は解消し、旧JSON/TSV境界に近い傾向へ戻った。

### Small select streaming

| shape | php-grpc-lite p50/p99 | ext-grpc p50/p99 |
|---|---:|---:|
| 1x100b | 43.7 / 541.5 μs | 96.5 / 570.2 μs |
| 1x1k | 43.2 / 403.7 μs | 91.2 / 725.6 μs |
| 1x4k | 48.7 / 768.2 μs | 93.2 / 890.8 μs |
| 1x10k | 45.8 / 829.3 μs | 106.3 / 996.7 μs |

- TSV: `var/bench-results/phase2-small-select-streaming-otel-php-boundary-small-select-20260510-140059.tsv`
- OTEL summaryはTSVと同じp50/p99を返した。

## Close Criteria

- DML unary p99がmainの旧JSON/TSV境界と同等の傾向に戻るか確認する。Done.
- small select streamingがOTEL spanでext-grpcと比較できる。Done.
- C内部telemetry依存なしでOTEL summaryが出る。Done.
