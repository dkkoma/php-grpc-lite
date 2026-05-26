# HTTP/2 SETTINGS benchmark (2026-05-27)

## Purpose

`0.0.10`で追加した初期HTTP/2 SETTINGSのdefault値と、ext-grpc 1.58で観測した値に寄せたvariantを比較する。

- default: `grpc_lite.http2_max_frame_size=16384`, `grpc_lite.http2_max_header_list_size=65536`
- ext-settings variant: `grpc_lite.http2_max_frame_size=4194304`, `grpc_lite.http2_max_header_list_size=16384`

## Environment

- Date: 2026-05-27
- Host: local Docker compose / OrbStack
- Server: Go test-server in compose
- Result source: OTEL spans summarized by `tools/benchmark/otelop-summary.php`
- Run ids:
  - `settings-0-0-10-20260527-074247`
  - `settings-large-meta-20260527-074321`

## Commands

```bash
BENCH_OTEL_RUN_ID=settings-0-0-10-20260527-074247 \
BENCH_IMPLEMENTATION=php-grpc-lite \
BENCH_IMPLEMENTATION_LABEL=php-grpc-lite-default \
./bench/run.sh spanner-shape

BENCH_OTEL_RUN_ID=settings-0-0-10-20260527-074247 \
BENCH_IMPLEMENTATION=php-grpc-lite \
BENCH_IMPLEMENTATION_LABEL=php-grpc-lite-ext-settings \
BENCH_PHP_EXTRA_INI_ARGS='-d grpc_lite.http2_max_frame_size=4194304 -d grpc_lite.http2_max_header_list_size=16384' \
./bench/run.sh spanner-shape

BENCH_OTEL_RUN_ID=settings-0-0-10-20260527-074247 \
BENCH_IMPLEMENTATION=ext-grpc \
BENCH_IMPLEMENTATION_LABEL=ext-grpc \
./bench/run.sh spanner-shape
```

同じ形で `tls-spanner-shape`, `large-streaming`, `metadata-header` も実行した。

## Spanner shape

| measurement | ext-grpc p50/p99 us | default p50/p99 us | ext-settings p50/p99 us |
|---|---:|---:|---:|
| begin_txn_unary | 86.8 / 671.0 | 33.1 / 98.9 | 30.4 / 163.3 |
| commit_txn_unary | 74.8 / 302.1 | 29.2 / 93.6 | 29.9 / 104.6 |
| dml_delete_10col_streaming | 72.5 / 112.7 | 31.5 / 74.7 | 32.5 / 290.8 |
| dml_insert_10col_streaming | 72.6 / 225.2 | 25.9 / 87.1 | 27.2 / 71.0 |
| dml_update_10col_streaming | 77.6 / 118.6 | 24.0 / 88.9 | 27.3 / 514.0 |
| select_1row_10col_streaming | 76.6 / 169.3 | 27.3 / 88.7 | 26.5 / 87.5 |

## TLS Spanner shape

| measurement | ext-grpc p50/p99 us | default p50/p99 us | ext-settings p50/p99 us |
|---|---:|---:|---:|
| begin_txn_unary | 77.2 / 322.1 | 30.2 / 97.7 | 31.3 / 244.3 |
| commit_txn_unary | 74.2 / 125.2 | 30.0 / 297.9 | 33.3 / 240.5 |
| dml_delete_10col_streaming | 75.5 / 126.2 | 32.2 / 223.9 | 28.2 / 107.6 |
| dml_insert_10col_streaming | 85.2 / 179.3 | 28.4 / 75.9 | 27.5 / 87.4 |
| dml_update_10col_streaming | 80.1 / 144.4 | 30.8 / 393.5 | 31.2 / 202.9 |
| select_1row_10col_streaming | 76.2 / 183.0 | 28.8 / 155.4 | 26.2 / 74.3 |

## Large streaming

`large-streaming`は各measurement count=1のため参考値。

| measurement | ext-grpc us | default us | ext-settings us |
|---|---:|---:|---:|
| large_streaming_count_10000 payload=100 | 29291.6 | 17562.5 | 8832.5 |
| large_streaming_count_100000 payload=100 | 241692.8 | 85333.9 | 69884.1 |

## Metadata/header

| measurement | ext-grpc p50/p99 us | default p50/p99 us | ext-settings p50/p99 us |
|---|---:|---:|---:|
| req_0_resp_0_value_0b | 118.5 / 2066.6 | 25.0 / 1888.6 | 42.0 / 1284.6 |
| req_10_resp_0_value_32b | 95.5 / 410.4 | 46.4 / 1287.1 | 60.9 / 96.0 |
| req_10_resp_10_value_32b | 102.0 / 416.3 | 48.0 / 2372.7 | 55.3 / 1140.2 |
| req_50_resp_0_value_32b | 123.0 / 396.5 | 78.7 / 633.0 | 84.0 / 618.3 |
| req_50_resp_50_value_32b | 212.3 / 3199.6 | 162.8 / 3266.4 | 163.9 / 2438.8 |

## Interpretation

- `0.0.10` default SETTINGS does not show a broad regression on controlled Spanner-shape benchmarks.
- ext-grpc相当SETTINGSはsmall shapeでは一貫した改善ではない。p50は同等から微悪化、p99は改善・悪化が混在した。
- large streamingではext-settingsが大きく改善したが、count=1の参考値であり、`MAX_FRAME_SIZE=4MiB`をdefault化する根拠にはしない。
- metadata/headerではext-settingsのp50はdefaultより遅いケースが多い。一方p99は外れ値の影響が大きく、このrunだけで結論を出さない。

## Decision

`0.0.10` defaultは維持する。`MAX_FRAME_SIZE=4194304`はlarge streaming向けの検証候補として残すが、small RPC主用途のdefaultにはしない。
