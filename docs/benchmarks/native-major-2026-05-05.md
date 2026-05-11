# HTTP/2 transport 主要ベンチ再計測 2026-05-05

## 条件

- 実行日: 2026-05-05
- 環境: local Docker compose / OrbStack
- 対向: `poc/test-server` Go gRPC/h2c/TLS fixtures
- tag: `20260505-major-native`
- php-grpc-lite: HTTP/2 transport
- 比較対象: official `ext-grpc`

## 実行

```bash
BENCH_TAG=20260505-major-native ./bench/run.sh cold
BENCH_TAG=20260505-major-native ./bench/run.sh warm
BENCH_TAG=20260505-major-native ./bench/run.sh stream-smoke
BENCH_TAG=20260505-major-native ./bench/run.sh tls
BENCH_TAG=20260505-major-native ./bench/preset.sh compare
```

`bench/run.sh` の php-grpc-lite 側は PHPBench remote executor の子PHPにも native extension を渡すため、`--php-config='{"extension":"/workspace/ext/grpc/modules/grpc.so"}'` を指定する。

## PHPBench

| case | php-grpc-lite mode | php-grpc-lite rstdev | ext-grpc mode | ext-grpc rstdev | 見解 |
|---|---:|---:|---:|---:|---|
| cold unary | 37.318μs | ±0.94% | 88.727μs | ±2.38% | nativeが速い |
| warm unary | 36.891μs | ±1.45% | 73.746μs | ±3.30% | nativeが速い |
| server streaming 1000 | 1.244ms | ±1.78% | 2.751ms | ±2.90% | nativeが速い |
| warm TLS unary | 44.538μs | ±2.34% | 88.978μs | ±2.82% | nativeが速い |
| cold TLS unary | 43.044μs | ±2.69% | 104.257μs | ±2.08% | nativeが速い |
| warm mTLS unary | 52.271μs | ±2.62% | 96.900μs | ±2.48% | nativeが速い |
| cold mTLS unary | 51.029μs | ±1.84% | 2.501ms | ±0.38% | nativeが大きく速い。ext-grpc側は証明書/handshake pathの固定費が大きい |

## Phase 2: unary

| case | php-grpc-lite calls/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| throughput unary 100B | 29,403 | 28.1μs | 67.8μs | 16,289 | 56.6μs | 103.6μs | nativeが速い |
| payload unary 0B | 29,361 | 28.0μs | 69.3μs | 16,437 | 52.7μs | 122.8μs | nativeが速い |
| payload unary 100B | 29,240 | 28.8μs | 66.0μs | 15,839 | 58.6μs | 106.9μs | nativeが速い |
| payload unary 1KiB | 29,327 | 28.2μs | 68.7μs | 15,471 | 60.8μs | 109.2μs | nativeが速い |
| payload unary 10KiB | 20,807 | 31.6μs | 473.5μs | 12,449 | 66.8μs | 561.2μs | nativeが速い |
| payload unary 100KiB | 5,504 | 77.9μs | 2,243.1μs | 5,458 | 106.7μs | 1,491.2μs | p50/throughputはnative、p99はext-grpcが良い |

## Phase 2: RTT unary

| case | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---|
| warm direct | 43.4μs | 57.4μs | 92.3μs | 132.6μs | nativeが速い |
| cold direct | 36.7μs | 44.9μs | 105.8μs | 239.3μs | nativeが速い |
| warm downstream 1ms | 1,393.0μs | 2,299.7μs | 1,978.2μs | 2,151.2μs | p50はnative、p99はext-grpc |
| cold downstream 1ms | 1,988.1μs | 2,081.4μs | 1,958.0μs | 2,047.4μs | ほぼ同等 |
| warm downstream 3ms | 4,363.1μs | 5,053.0μs | 4,378.3μs | 5,028.4μs | ほぼ同等 |
| cold downstream 3ms | 4,304.7μs | 5,057.9μs | 4,405.7μs | 5,070.4μs | ほぼ同等 |
| warm downstream 5ms | 6,391.8μs | 7,040.1μs | 6,476.7μs | 7,061.4μs | ほぼ同等 |
| cold downstream 5ms | 6,148.2μs | 6,954.1μs | 6,526.2μs | 7,523.2μs | nativeがやや良い |

## Phase 2: server streaming

| case | php-grpc-lite msg/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc msg/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| throughput streaming 100x100B | 648,945 | 135.8μs | 538.8μs | 286,357 | 334.1μs | 796.8μs | nativeが速い |
| payload streaming 0B | 286,228 | 161.8μs | 1,154.1μs | 169,854 | 386.3μs | 1,365.6μs | nativeが速い |
| payload streaming 100B | 643,960 | 151.5μs | 174.1μs | 286,392 | 362.8μs | 370.2μs | nativeが速い |
| payload streaming 1KiB | 439,254 | 226.2μs | 239.3μs | 246,772 | 427.3μs | 435.6μs | nativeが速い |
| payload streaming 10KiB | 78,123 | 1,111.2μs | 2,255.9μs | 71,098 | 1,452.6μs | 1,974.3μs | p50/throughputはnative、p99はext-grpc |

| case | php-grpc-lite msg/s | ext-grpc msg/s | 見解 |
|---|---:|---:|---|
| large streaming 1000x100B | 477,270 | 290,082 | nativeが速い |
| large streaming 10000x100B | 786,945 | 386,043 | nativeが速い |

## Phase 2: metadata

| case | php-grpc-lite calls/s | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc calls/s | ext-grpc p50 | ext-grpc p99 | 見解 |
|---|---:|---:|---:|---:|---:|---:|---|
| req0 / resp0 / 0B | 5,411 | 60.0μs | 1,249.5μs | 5,200 | 98.5μs | 1,015.9μs | p50はnative、p99はext-grpc |
| req10 / resp0 / 32B | 19,275 | 47.7μs | 76.3μs | 10,493 | 89.2μs | 131.1μs | nativeが速い |
| req50 / resp0 / 32B | 6,311 | 125.9μs | 474.6μs | 5,538 | 133.2μs | 446.9μs | p50はほぼ同等、p99はext-grpc |
| req10 / resp10 / 32B | 15,377 | 56.8μs | 89.5μs | 10,282 | 84.5μs | 125.1μs | nativeが速い |
| req50 / resp50 / 32B | 2,955 | 216.8μs | 1,398.6μs | 3,543 | 218.7μs | 775.9μs | ext-grpcが良い |

## 見解

- small unary / small server streaming は native が ext-grpc より良い。
- RTTが数ms入るとtransport差はほぼ埋もれる。
- large unary 100KiB と metadata大量系では native の p99 が ext-grpc より悪いケースが残る。
- server streamingは単一streamの同期blocking利用では native が概ね優位。mux対応の価値は、単一call高速化よりも同一process/thread内で複数in-flight RPCを扱う実行モデルが必要かで判断するのが妥当。

## 生成物

- `var/bench-results/*20260505-major-native*.json`
- `var/bench-results/*20260505-major-native*.tsv`
- `var/bench-results/*20260505-major-native*.log`
