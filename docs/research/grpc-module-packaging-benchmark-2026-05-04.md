# grpc module packaging後の主要ベンチ再取得 (2026-05-04)

## 目的

native extensionを `grpc.so` / module名 `grpc` としてbuild/loadする案Aへ切り替えた後、curl / native / ext-grpc の主要ベンチを取り直す。

この計測はpackaging変更による性能退行確認であり、ext-grpcの値に近づけること自体を目的にしない。

## 実行条件

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && phpize >/tmp/grpc-phpize.log && ./configure --enable-grpc >/tmp/grpc-configure.log && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_export([extension_loaded("grpc"), function_exists("grpc_native_persistent_channel_unary")]); echo PHP_EOL;'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit tests/Integration/NativeTransportControlTest.php tests/Integration/MetadataControlTest.php tests/Integration/MetadataCompatibilityTest.php tests/Integration/CallCredentialsTest.php tests/Integration/ControlSemanticsTest.php tests/Integration/TransportSelectionTest.php tests/Integration/BinaryMetadataTest.php
ITERATIONS=10 BENCH_TAG=grpc-module-smoke ./bench/phase2/check-native-lifecycle-stress.sh
BENCH_TAG=grpc-module-small-select ./bench/phase2/compare-small-select-streaming.sh
BENCH_TAG=grpc-module-spanner-dml ./bench/phase2/compare-spanner-dml-unary-shape.sh
BENCH_TAG=grpc-module-major ./bench/phase2/compare-native-mvp-vs-libcurl-ext.sh
```

確認:

- `extension_loaded("grpc")`: true
- `function_exists("grpc_native_persistent_channel_unary")`: true
- targeted PHPUnit: 52 tests / 143 assertions / 1 skipped
- native lifecycle smoke: 10 iterations, all scenarios failures=0

## Small SELECT Streaming

summary: `var/bench-results/phase2-small-select-streaming-grpc-module-small-select.tsv`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| 1x100B | 210.7 / 1812.6 μs | 43.2 / 362.7 μs | 80.3 / 480.9 μs | nativeが最良 |
| 1x1KiB | 212.9 / 1507.6 μs | 45.8 / 432.0 μs | 85.5 / 742.7 μs | nativeが最良 |
| 1x4KiB | 218.4 / 1557.6 μs | 44.3 / 639.8 μs | 84.9 / 420.8 μs | p50はnative最良、p99はext-grpc最良 |
| 1x10KiB | 219.2 / 1824.1 μs | 46.6 / 720.5 μs | 92.6 / 1086.9 μs | nativeが最良 |

small SELECT代表形状では、packaging後もnativeはcurl/ext-grpcより概ね優位。`1x4KiB` のp99のみext-grpcが良い。

## Spanner DML Unary Shape

summary: `var/bench-results/phase2-spanner-dml-unary-shape-grpc-module-spanner-dml.tsv`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| begin_txn | 39.6 / 120.3 μs | 30.8 / 148.3 μs | 53.7 / 103.0 μs | p50はnative最良、p99はext-grpc最良 |
| dml_insert_10col | 39.6 / 87.2 μs | 29.5 / 68.3 μs | 54.6 / 105.8 μs | nativeが最良 |
| dml_update_10col | 40.3 / 163.6 μs | 30.1 / 69.9 μs | 55.5 / 101.4 μs | nativeが最良 |
| dml_delete_10col | 40.2 / 127.9 μs | 29.8 / 67.7 μs | 60.0 / 100.3 μs | nativeが最良 |
| commit_txn | 39.8 / 85.7 μs | 29.9 / 67.5 μs | 56.2 / 107.7 μs | nativeが最良 |

small unary代表形状では、nativeはp50で一貫して最良。p99は `begin_txn` だけext-grpcが良いが、他はnativeが最良。

## Phase2 Main Comparison

summary: `var/bench-results/phase2-native-mvp-vs-libcurl-ext-grpc-module-major.tsv`

| case | curl p50/p99 | native p50/p99 | ext-grpc p50/p99 | 見解 |
|---|---:|---:|---:|---|
| large request unary 1MiB | 865.2 / 4936.1 μs | 682.1 / 4557.9 μs | 429.5 / 3575.0 μs | ext-grpcが最良、nativeはcurlより改善 |
| 1000x100B stream | 4898.0 / 8438.1 μs | 4326.9 / 9336.4 μs | 5205.2 / 9656.8 μs | p50/throughputはnative最良、p99はcurl最良 |
| 10x100KiB stream | 932.0 / 4451.4 μs | 766.7 / 4389.2 μs | 647.8 / 3641.1 μs | ext-grpcが最良、nativeはcurlより改善 |
| 100x100KiB stream | 5458.7 / 12282.1 μs | 9273.0 / 16258.5 μs | 5631.6 / 9131.0 μs | native surfaceが悪化、継続課題 |
| 1x1MiB stream | 927.7 / 4301.9 μs | 751.8 / 5071.3 μs | 564.0 / 3132.5 μs | ext-grpcが最良、nativeはp50のみcurlより改善 |
| 10000x100B stream | 43652.2 / 57075.8 μs | 40508.8 / 52496.7 μs | 42953.0 / 49250.0 μs | p50/throughputはnative最良、p99はext-grpc最良 |

## 判断

- 案Aの `grpc.so` packaging後も、主用途のsmall unary / small SELECT streamingではnative defaultを維持できる数値。
- `grpc.so` 化そのものによる明確な退行は見えていない。
- large responseでは `100x100KiB` のnative surface悪化が残る。release defaultの主用途からは外し、transport selection guideのknown limitationとして扱う。
- large bulk streamingの運用閾値は、1 message `>=64KiB` かつ stream total `>=8MiB`、またはlarge payload `>=50 messages` とする。この閾値を超えてp99/throughputがSLOに入る場合は `native` / `curl` を事前比較する。
- release gateとしては、packagingは「module名、build target、source build手順は完了、CI matrixは継続」。次はnative memory checker、FPM/worker lifecycle QAを詰める。
