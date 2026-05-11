# OTEL span基準でSpanner shapeを再計測する

- Status: Closed
- Created: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

`BenchTelemetry` を共通span recorderへ移行したため、以前の同期OTLP export込みTSVではなく、OTEL span durationを一次ソースとしてSpanner shapeを再計測する。

## Scope

- Spanner DML unary shape: `php-grpc-lite` vs `ext-grpc`, 各case 1000 calls。
- Small select streaming shape: 1 stream = 1 message, payload 100B / 1KiB / 4KiB / 10KiB, 各case 1000 streams。
- `otelop-summary.php` の出力を判断値にする。

## Verification

### DML unary

Command:

```bash
BENCH_TAG=otel-spanner-dml-20260510-133342 \
BENCH_OTEL_RUN_ID=otel-spanner-dml-20260510-133342 \
BENCH_OTEL_EXPORTER=otlp-http \
BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 \
BENCH_OTEL_SUMMARY_LIMIT=20000 \
./bench/compare-spanner-dml-unary-shape.sh
```

OTEL summary:

| case | grpc-lite p50/p99 us | ext-grpc p50/p99 us | note |
|---|---:|---:|---|
| begin_txn | 35.3 / 306.2 | 62.4 / 463.1 | grpc-lite優位 |
| dml_insert_10col | 37.2 / 328.8 | 65.0 / 268.0 | p50 grpc-lite優位、p99 ext-grpc優位 |
| dml_update_10col | 43.5 / 755.2 | 71.9 / 302.3 | p50 grpc-lite優位、p99 ext-grpc優位 |
| dml_delete_10col | 35.2 / 268.2 | 71.6 / 123.2 | p50 grpc-lite優位、p99 ext-grpc優位 |
| commit_txn | 42.9 / 376.8 | 71.4 / 114.3 | p50 grpc-lite優位、p99 ext-grpc優位 |

JSON/TSV compatibility output:

- `var/bench-results/phase2-spanner-dml-unary-shape-otel-spanner-dml-20260510-133342.tsv`
- `var/bench-results/phase2-spanner-dml-unary-shape-otel-spanner-dml-20260510-133342-native.json`
- `var/bench-results/phase2-spanner-dml-unary-shape-otel-spanner-dml-20260510-133342-ext-grpc.json`

### Small select streaming

Command:

```bash
BENCH_TAG=otel-small-select-20260510-133405 \
BENCH_OTEL_RUN_ID=otel-small-select-20260510-133405 \
BENCH_OTEL_EXPORTER=otlp-http \
BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
WARMUP_STREAMS=10 INCLUDE_FRANKEN=0 INCLUDE_POC=0 \
BENCH_OTEL_SUMMARY_LIMIT=20000 \
./bench/compare-small-select-streaming.sh
```

OTEL summary:

| shape | grpc-lite p50/p99 us | ext-grpc p50/p99 us | note |
|---|---:|---:|---|
| payload=100 | 48.4 / 506.2 | 94.1 / 884.7 | grpc-lite優位 |
| payload=1024 | 51.8 / 309.9 | 100.0 / 628.3 | grpc-lite優位 |
| payload=4096 | 52.5 / 480.0 | 93.7 / 879.8 | grpc-lite優位 |
| payload=10240 | 53.3 / 1150.5 | 101.1 / 1157.6 | p50 grpc-lite優位、p99同等 |

JSON/TSV compatibility output:

- `var/bench-results/phase2-small-select-streaming-otel-small-select-20260510-133405.tsv`

## Decision Log

- OTEL span durationではsmall select streamingのp50はgrpc-liteがext-grpcより明確に速い。
- DML unaryはp50ではgrpc-lite優位。p99はcaseによりext-grpc優位が残るため、tail要因を見るならgrpc-lite internal p99とserver statsを掘る。
- JSON/TSVは互換出力として残るが、今回の判断値はOTEL summary。

## Close Criteria

- DML unaryを1000 callsで再計測した。Done.
- Small select streamingを1000 streamsで再計測した。Done.
- OTEL summaryを記録した。Done.
