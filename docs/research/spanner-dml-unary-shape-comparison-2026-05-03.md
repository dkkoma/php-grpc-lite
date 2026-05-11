# Spanner DML Unary Shape Comparison 2026-05-03

## Purpose

Spanner emulatorで観測したDML flowのunary request/responseサイズを、Go test-serverのsynthetic `BenchUnary` で比較できるようにした。

これはSpanner emulator自体の性能測定ではない。DMLで実際に発生する「小さいunary callの連続」に近いmessage sizeを、`php-grpc-lite curl` / `php-grpc-lite native` / `ext-grpc` で比較するためのベンチである。

## Command

```bash
DURATION=0.2 WARMUP_CALLS=1 MAX_CALLS=50 \
BENCH_TAG=20260503-spanner-dml-unary-shape \
bench/compare-spanner-dml-unary-shape.sh
```

短時間のsanity runなので、tail判断には長めのrepeatが必要。

## Shape Source

| case | request bytes | response bytes | source |
|---|---:|---:|---|
| begin_txn | 92 | 18 | `BeginTransaction` |
| dml_insert_10col | 355 | 8 | `ExecuteSql` DML insert |
| dml_update_10col | 327 | 8 | `ExecuteSql` DML update |
| dml_delete_10col | 144 | 8 | `ExecuteSql` DML delete |
| commit_txn | 106 | 14 | `Commit` |

## Results

| case | implementation | variant | p50 us | p99 us | calls/s |
|---|---|---|---:|---:|---:|
| begin_txn | php-grpc-lite | curl | 49.8 | 122.2 | 18972.1 |
| dml_insert_10col | php-grpc-lite | curl | 46.3 | 77.1 | 20179.3 |
| dml_update_10col | php-grpc-lite | curl | 44.7 | 489.4 | 16300.6 |
| dml_delete_10col | php-grpc-lite | curl | 42.0 | 57.4 | 23197.5 |
| commit_txn | php-grpc-lite | curl | 40.8 | 54.5 | 24286.8 |
| begin_txn | php-grpc-lite | native | 155.6 | 1372.6 | 4591.3 |
| dml_insert_10col | php-grpc-lite | native | 149.8 | 1803.1 | 4550.0 |
| dml_update_10col | php-grpc-lite | native | 127.4 | 1394.0 | 5709.0 |
| dml_delete_10col | php-grpc-lite | native | 131.9 | 1643.7 | 5009.6 |
| commit_txn | php-grpc-lite | native | 130.8 | 624.2 | 6608.7 |
| begin_txn | ext-grpc | c-core | 76.0 | 141.7 | 12424.5 |
| dml_insert_10col | ext-grpc | c-core | 51.4 | 73.7 | 18761.2 |
| dml_update_10col | ext-grpc | c-core | 52.5 | 1617.1 | 10169.7 |
| dml_delete_10col | ext-grpc | c-core | 49.9 | 71.8 | 19115.0 |
| commit_txn | ext-grpc | c-core | 56.5 | 97.9 | 17203.1 |

## Interpretation

- DML shapeは既存のlarge request/large response benchmarkではなく、small unary固定費の問題である。
- curl routeはこの短時間runではext-grpcと同等または一部で速い。既存のcurl handle reuseが効く範囲。
- native actual surfaceはsmall unaryでまだ遅い。small SELECT streamingと同じく、nativeをrelease defaultにするにはsmall pathの固定費改善が必要。
- p99は50 callsの短時間runでは不安定なので、decision runではduration/repeatsを増やす。
