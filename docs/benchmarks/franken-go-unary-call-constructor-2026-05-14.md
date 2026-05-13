# Franken-go UnaryCall constructor benchmark 2026-05-14

## Context

`github.com/dkkoma/frankenphp-grpc-go-client` の `FrankenGrpc\UnaryCall::__construct()` 軽量化後の `franken-go` backend を、php-grpc-lite `main` の native backend と公式 ext-grpc に並べて再計測した。

- php-grpc-lite base: `5b1b27d` の次作業中。計測コードの追加変更なし。
- frankenphp-grpc-go-client: `github.com/dkkoma/frankenphp-grpc-go-client@main` resolved by xcaddy as `v0.0.0-20260513215327-f290d0169347`
- previous franken-go comparison: `docs/benchmarks/franken-go-channel-pool-2026-05-13.md`
- server: compose `test-server`
- summary source: OTEL spans in `otelop`, summarized by `tools/benchmark/otelop-summary.php`

`FrankenGrpc\UnaryCall` は php-grpc-lite の franken-go unary pathでRPCごとに生成されるため、warm channelでも unary call固定費として計測に出る想定。

## Verification commands

```bash
docker compose build --no-cache dev-franken-grpc-go
docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh
```

```bash
BENCH_TAG=franken-unary-ctor-dml-20260514-070000 \
BENCH_OTEL_RUN_ID=franken-unary-ctor-dml-20260514-070000 \
INCLUDE_FRANKEN=1 \
DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 \
BENCH_OTEL_SUMMARY_LIMIT=20000 \
./bench/compare-spanner-dml-unary-shape.sh
```

```bash
BENCH_TAG=franken-unary-ctor-throughput-20260514-070100 \
BENCH_OTEL_RUN_ID=franken-unary-ctor-throughput-20260514-070100 \
./bench/run.sh throughput-unary --duration=0.2 --payload-bytes=100
```

## Spanner DML unary shape

| case | native p50/p99 us | franken-go p50/p99 us | ext-grpc p50/p99 us | previous franken-go p50/p99 us | note |
|---|---:|---:|---:|---:|---|
| begin_txn | 32.0 / 463.7 | 65.5 / 678.8 | 58.4 / 220.7 | 57.6 / 330.9 | 悪化。tailの揺れが大きい |
| dml_insert_10col | 31.8 / 182.1 | 51.2 / 282.7 | 57.7 / 129.2 | 60.9 / 187.5 | p50改善、p99悪化 |
| dml_update_10col | 29.0 / 133.6 | 67.5 / 209.9 | 60.6 / 163.3 | 72.5 / 216.7 | 小幅改善 |
| dml_delete_10col | 28.1 / 143.2 | 70.8 / 180.2 | 63.0 / 161.5 | 72.1 / 240.2 | 小幅改善 |
| commit_txn | 29.1 / 103.3 | 67.2 / 288.9 | 62.5 / 150.5 | 70.6 / 323.5 | 小幅改善 |

## Throughput unary spot check

| suite | shape | native p50/p99 us | franken-go p50/p99 us | ext-grpc p50/p99 us | previous franken-go p50/p99 us | note |
|---|---|---:|---:|---:|---:|---|
| throughput-unary | payload=100, duration=0.2s | 28.0 / 127.5 | 62.7 / 391.9 | 57.2 / 211.9 | 61.0 / 385.1 | ほぼ変化なし |

## Conclusion

- `UnaryCall::__construct()` 軽量化は、今回のPHP runner外側RPC spanでは明確な性能改善としては見えていない。
- DML unaryではcaseによりfranken-go p50が小幅改善しているが、begin_txnやp99は悪化しており、ノイズを超えた一貫した改善とは判断できない。
- throughput-unaryでは前回 `61.0 / 385.1us`、今回 `62.7 / 391.9us` でほぼ同等。
- main native backend は今回も small unary p50 で最速。franken-goをdefault化する根拠はない。
- constructor軽量化の効果だけを分離したい場合は、RPC全体ではなく `FrankenGrpc\UnaryCall` object生成だけを測る専用micro benchmarkが必要。ただし実用判断としてはRPC全体spanで有意差が出ていない。
