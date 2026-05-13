# Spanner shape benchmark 2026-05-14

## 目的

`google/cloud-spanner` 実経路はGAX、session、transaction、Result materialization、Spanner emulatorの固定費が混ざる。transport寄りの主要性能観測として、Spanner代表RPC shapeをGo test-server上で制御可能に測る。

## 条件

- suite: `spanner-shape`
- command: `BENCH_TAG=spanner-shape-20260514 ./bench/compare.sh spanner-shape`
- target: `test-server:50051`
- calls: 1000 per measurement
- warmup calls: 10 per measurement
- primary source: OTEL spans summarized by `tools/benchmark/otelop-summary.php`

## shape

| measurement | call type | request bytes | response bytes | message count | 想定 |
| --- | --- | ---: | ---: | ---: | --- |
| `begin_txn_unary` | unary | 92 | 18 | - | `BeginTransaction` |
| `commit_txn_unary` | unary | 106 | 14 | - | `Commit` |
| `select_1row_10col_streaming` | server streaming | 160 | 100 | 1 | small `ExecuteStreamingSql` SELECT |
| `dml_insert_10col_streaming` | server streaming | 355 | 8 | 1 | DML insert `ExecuteStreamingSql` stats |
| `dml_update_10col_streaming` | server streaming | 327 | 8 | 1 | DML update `ExecuteStreamingSql` stats |
| `dml_delete_10col_streaming` | server streaming | 144 | 8 | 1 | DML delete `ExecuteStreamingSql` stats |

## 結果

| measurement | grpc-lite native p50 | grpc-lite native p99 | ext-grpc p50 | ext-grpc p99 | 見解 |
| --- | ---: | ---: | ---: | ---: | --- |
| begin_txn_unary | 31.6µs | 128.6µs | 59.2µs | 393.4µs | nativeが優位 |
| commit_txn_unary | 28.2µs | 83.4µs | 54.9µs | 107.0µs | nativeが優位 |
| select_1row_10col_streaming | 28.9µs | 70.9µs | 62.3µs | 115.6µs | nativeが優位 |
| dml_insert_10col_streaming | 26.1µs | 69.0µs | 68.1µs | 110.4µs | nativeが優位 |
| dml_update_10col_streaming | 26.1µs | 72.5µs | 68.5µs | 110.3µs | nativeが優位 |
| dml_delete_10col_streaming | 27.3µs | 65.4µs | 71.1µs | 128.9µs | nativeが優位 |

## 判断

- `spanner-shape` は主要性能ベンチとして残す。
- `spanner-real-client` は実アプリ経路のsmoke/regressionとして残す。
- Spanner DML shapeはunaryではなくserver streamingとして扱う。
