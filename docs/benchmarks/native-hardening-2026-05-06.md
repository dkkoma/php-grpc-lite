# HTTP/2 transport hardening後 主要ベンチ再計測 2026-05-06

## 条件

- 実行日: 2026-05-06
- 環境: local Docker compose / OrbStack
- 対向: `poc/test-server` Go gRPC/h2c/TLS fixtures
- tag: `20260506-native-hardening`
- php-grpc-lite: bundled `grpc` extension / HTTP/2 transport
- 比較対象: official `ext-grpc`

## 実行

```bash
BENCH_TAG=20260506-native-hardening ./bench/run.sh cold
BENCH_TAG=20260506-native-hardening ./bench/run.sh warm
BENCH_TAG=20260506-native-hardening ./bench/run.sh stream-smoke
BENCH_TAG=20260506-native-hardening ./bench/run.sh tls
BENCH_TAG=20260506-native-hardening ./bench/preset.sh compare
```

`bench/preset.sh compare` は、汎用 transport ベンチに加えて Spanner 主用途の shape ベンチも実行する。

## PHPBench

| case | php-grpc-lite mode | php-grpc-lite rstdev | ext-grpc mode | ext-grpc rstdev | 見解 |
|---|---:|---:|---:|---:|---|
| cold unary | 43.840μs | ±2.04% | 89.884μs | ±0.96% | nativeが速い |
| warm unary | 36.241μs | ±1.76% | 70.870μs | ±3.55% | nativeが速い |
| server streaming 1000 | 1.201ms | ±3.70% | 2.645ms | ±1.93% | nativeが速い |
| warm TLS unary | 53.800μs | ±2.02% | 89.306μs | ±3.26% | nativeが速い |
| cold TLS unary | 54.390μs | ±2.13% | 114.435μs | ±2.62% | nativeが速い |
| warm mTLS unary | 67.861μs | ±0.99% | 94.845μs | ±1.51% | nativeが速い |
| cold mTLS unary | 76.745μs | ±2.07% | 2.519ms | ±2.28% | nativeが大きく速い |

## Phase 2: unary

| case | php-grpc-lite calls/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| throughput unary 100B | 27,073 | 31.2μs | 78.2μs | 15,281 | 61.0μs | 110.9μs | nativeが速い |
| payload unary 0B | 27,197 | 30.8μs | 74.8μs | 16,002 | 55.1μs | 121.3μs | nativeが速い |
| payload unary 100B | 26,795 | 32.0μs | 73.8μs | 15,735 | 58.8μs | 102.5μs | nativeが速い |
| payload unary 1KiB | 27,558 | 31.0μs | 73.0μs | 15,106 | 58.9μs | 162.2μs | nativeが速い |
| payload unary 10KiB | 19,843 | 32.1μs | 319.3μs | 12,181 | 67.7μs | 528.6μs | nativeが速い |
| payload unary 100KiB | 4,918 | 101.3μs | 2,316.8μs | 5,233 | 114.6μs | 1,539.7μs | p50はnative、throughput/p99はext-grpc |

## Phase 2: RTT unary

| case | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---|
| warm direct | 57.4μs | 83.4μs | 90.6μs | 135.1μs | nativeが速い |
| cold direct | 43.3μs | 2,103.1μs | 70.0μs | 98.8μs | native側p99に外れ値 |
| warm downstream 1ms | 1,967.3μs | 2,164.1μs | 1,978.3μs | 2,083.1μs | ほぼ同等 |
| cold downstream 1ms | 1,740.1μs | 2,031.5μs | 1,967.9μs | 3,028.4μs | nativeがやや良い |
| warm downstream 3ms | 4,354.8μs | 5,122.1μs | 4,497.4μs | 5,017.7μs | ほぼ同等 |
| cold downstream 3ms | 4,474.4μs | 4,981.1μs | 4,038.3μs | 5,016.4μs | ほぼ同等 |
| warm downstream 5ms | 6,933.5μs | 7,044.1μs | 6,396.0μs | 7,012.2μs | ほぼ同等 |
| cold downstream 5ms | 6,435.7μs | 7,004.8μs | 6,207.9μs | 6,947.8μs | ほぼ同等 |

## Phase 2: server streaming

| case | php-grpc-lite msg/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc msg/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| throughput streaming 100x100B | 636,511 | 137.2μs | 909.2μs | 274,753 | 338.8μs | 1,121.0μs | nativeが速い |
| payload streaming 0B | 282,629 | 129.2μs | 1,213.0μs | 101,018 | 1,044.5μs | 1,822.0μs | nativeが速い |
| payload streaming 100B | 616,073 | 168.0μs | 194.0μs | 255,855 | 388.5μs | 424.0μs | nativeが速い |
| payload streaming 1KiB | 370,630 | 267.0μs | 329.0μs | 231,935 | 430.5μs | 475.0μs | nativeが速い |
| payload streaming 10KiB | 64,204 | 907.6μs | 3,048.0μs | 68,635 | 1,332.8μs | 1,884.0μs | p50はnative、throughput/p99はext-grpc |

| case | php-grpc-lite msg/s | ext-grpc msg/s | 見解 |
|---|---:|---:|---|
| large streaming 1000x100B | 448,761 | 254,807 | nativeが速い |
| large streaming 10000x100B | 570,541 | 420,184 | nativeが速い |

## Phase 2: Spanner small SELECT shape

1 stream = 1 message の小さな server streaming response。Spanner `ExecuteStreamingSql` が 1 `PartialResultSet` で返る小さな SELECT を近似する。

| case | php-grpc-lite p50 | php-grpc-lite p99 | php-grpc-lite msg/s | ext-grpc p50 | ext-grpc p99 | ext-grpc msg/s | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| 1x100B | 48.5μs | 559.0μs | 13,102 | 105.5μs | 863.2μs | 6,722 | nativeが速い |
| 1x1KiB | 58.0μs | 552.7μs | 12,442 | 115.3μs | 899.3μs | 6,159 | nativeが速い |
| 1x4KiB | 62.0μs | 664.0μs | 10,989 | 112.2μs | 2,016.8μs | 4,897 | nativeが速い |
| 1x10KiB | 63.4μs | 760.9μs | 9,941 | 116.6μs | 988.8μs | 5,654 | nativeが速い |

## Phase 2: Spanner DML unary shape

Spanner DML flow の unary request/response shape。10列テーブルへの DML と transaction control を近似する。

| case | req | resp | php-grpc-lite calls/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| begin_txn | 92B | 18B | 25,491 | 33.2μs | 104.2μs | 16,062 | 57.0μs | 111.9μs | nativeが速い |
| dml_insert_10col | 355B | 8B | 26,545 | 31.9μs | 72.5μs | 15,852 | 58.3μs | 105.8μs | nativeが速い |
| dml_update_10col | 327B | 8B | 25,954 | 32.2μs | 73.5μs | 15,802 | 59.0μs | 104.7μs | nativeが速い |
| dml_delete_10col | 144B | 8B | 26,059 | 31.9μs | 76.2μs | 16,029 | 58.1μs | 99.3μs | nativeが速い |
| commit_txn | 106B | 14B | 26,836 | 31.8μs | 70.1μs | 15,539 | 60.6μs | 107.1μs | nativeが速い |

## Phase 2: metadata

| case | php-grpc-lite calls/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| req0 / resp0 / 0B | 5,883 | 47.4μs | 1,209.0μs | 4,760 | 100.8μs | 1,037.5μs | p50はnative、p99はext-grpc |
| req10 / resp0 / 32B | 17,840 | 50.8μs | 85.5μs | 10,346 | 84.8μs | 131.7μs | nativeが速い |
| req50 / resp0 / 32B | 7,425 | 118.2μs | 180.6μs | 8,387 | 116.6μs | 147.7μs | ext-grpcがやや良い |
| req10 / resp10 / 32B | 14,188 | 60.1μs | 120.3μs | 5,307 | 91.5μs | 968.7μs | nativeが速い |
| req50 / resp50 / 32B | 3,002 | 246.3μs | 769.9μs | 4,020 | 250.4μs | 293.4μs | p50は同等、throughput/p99はext-grpc |

## 見解

- hardening後も small unary / small server streaming は native が ext-grpc より速い。
- Spanner主用途の small SELECT shape と DML unary shape でも native が ext-grpc より速い。
- TLS/mTLS unary は native が優位。cold mTLS の ext-grpc は今回も大きい固定費が出ている。
- 100KiB unary、10KiB streaming、metadata大量系では ext-grpc が throughput または p99 で優位なケースが残る。
- RTTが数ms入るケースではtransport差は概ね埋もれる。0ms cold direct の native p99 は単発外れ値として扱い、必要なら再計測する。
- `20260505-major-native` と比べると、nativeの小型unaryはやや遅く、streaming throughputも一部低下しているが、主要な優劣関係は維持されている。

## 生成物

- `var/bench-results/*20260506-native-hardening*.json`
- `var/bench-results/*20260506-native-hardening*.tsv`
- `var/bench-results/*20260506-native-hardening*.log`
