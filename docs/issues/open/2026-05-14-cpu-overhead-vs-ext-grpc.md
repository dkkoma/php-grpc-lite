---
Status: Open
Owner: Codex
Created: 2026-05-14
Branch: main
---

# ext-grpc比CPU使用率差分の原因特定

## 目的

実アプリケーション負荷試験で `php-grpc-lite` が `ext-grpc` と同等latencyながらCPU使用率が約1.5倍になる問題を、実用投入前に原因特定し改善する。

## 背景

ユーザーの実アプリケーション負荷試験では、速度は `ext-grpc` と同等だがCPU使用率が約1.5倍という結果が出ている。latencyが同等でもCPU固定費が高いとFPM/workerの収容効率が落ち、実用上の制約になる。

## スコープ

- 実アプリケーション条件に近い代表RPCでCPU差分を再現する。
- `ext-grpc` / `php-grpc-lite` のlatency、RPS、CPU time、wall timeを同条件で比較する。
- C extension内のCPU hot pathを特定する。
- PHP userland、protobuf serialize/deserialize、metadata変換、nghttp2 send/recv loop、OpenSSL/TCP I/O待ちを分けて見る。
- 改善候補を小さいbranch単位で検証し、採用可否をbenchと実アプリ条件で判断する。

## 非スコープ

- ext-grpcの性能に合わせること自体を目的にしない。
- latencyだけの改善を目的にしない。
- 実アプリ側のビジネスロジック最適化。

## 初期仮説

- nghttp2 session send/recv loopの呼び出し回数やpoll/read/writeの粒度がCPU固定費になっている。
- response metadata / status / protobuf deliveryのC↔PHP境界回数が多い。
- persistent connection preflightやdeadline/socket timeout設定がcallごとにCPUを使っている。
- userland wrapper / GAX経路でnative extension以外の固定費が増えている。
- ext-grpcはC-core内のcompletion queue / batch処理で固定費をまとめている可能性がある。

## 計画

1. 実アプリ負荷試験の条件を記録する。
2. 制御可能なGo test-server上のmicro benchでCPU指標を追加取得する。
3. 既存主要ベンチと対応する代表形状をCPU観測対象にする。
4. `perf` / `callgrind` / PHP profilerのどれで見るか決める。
5. C hot pathを関数単位に分解する。
6. 改善候補をissue分割して検証する。

## 進捗

- `cpu-micro` suiteを追加した。
- `getrusage()` のuser/sys CPU timeをwarmup後の測定loop全体で取得し、`cpu_us/call`、`user_us/call`、`sys_us/call`、`wall_us/call` を出す。
- 測定loop内ではRPCごとのOTEL span生成を行わず、計測コード自体のCPUノイズを抑える。結果は `CpuMicroSummary` spanとしてOTELへexportする。
- `tools/benchmark/otelop-summary.php` は `benchmark.metric_kind=cpu_summary` を別表で集計する。

## CPU micro対象

- `small_unary_100b`
- `begin_txn_unary`
- `commit_txn_unary`
- `small_streaming_1x100b`
- `small_streaming_100x100b`
- `select_1row_10col_streaming`
- `dml_insert_10col_streaming`
- `dml_update_10col_streaming`
- `dml_delete_10col_streaming`

## 検証

- `docker compose run --rm dev php -l tools/benchmark/cpu-micro.php`
- `docker compose run --rm dev php -l tools/benchmark/BenchTelemetry.php`
- `docker compose run --rm dev php -l tools/benchmark/otelop-summary.php`
- `BENCH_TAG=cpu-micro-smoke BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-micro --calls=3 --warmup-calls=1`
- `BENCH_TAG=cpu-micro-verify BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-micro --calls=100 --warmup-calls=10`
- `BENCH_TAG=cpu-micro-20260514-203044 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-micro --calls=5000 --warmup-calls=100`
- `BENCH_TAG=cpu-real-20260514-203520 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-spanner-real-client --calls=100 --warmup-calls=5`
- `BENCH_TAG=cpu-concurrent-4w-20260514-204000 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-concurrent --workers=4 --calls=5000 --warmup-calls=100`
- `BENCH_TAG=cpu-concurrent-8w-20260514-204000 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-concurrent --workers=8 --calls=3000 --warmup-calls=100`
- `BENCH_TAG=cpu-micro-lifecycle-20260514-204100 BENCH_OTEL_SUMMARY_LIMIT=100000 ./bench/compare.sh cpu-micro --calls=3000 --warmup-calls=100`

## 2026-05-14 CPU micro計測結果

run id: `cpu-micro-20260514-203044`

| measurement | native cpu_us/call | ext-grpc cpu_us/call | native/ext | native wall_us/call | ext-grpc wall_us/call |
|---|---:|---:|---:|---:|---:|
| small_unary_100b | 11.3 | 39.5 | 0.29x | 31.4 | 65.8 |
| begin_txn_unary | 10.5 | 40.5 | 0.26x | 29.4 | 66.4 |
| commit_txn_unary | 10.9 | 40.7 | 0.27x | 31.4 | 65.8 |
| small_streaming_1x100b | 11.2 | 45.3 | 0.25x | 30.1 | 70.8 |
| small_streaming_100x100b | 54.5 | 266.5 | 0.20x | 136.0 | 345.4 |
| select_1row_10col_streaming | 11.5 | 45.3 | 0.25x | 30.8 | 73.1 |
| dml_insert_10col_streaming | 11.5 | 46.2 | 0.25x | 32.5 | 71.1 |
| dml_update_10col_streaming | 11.1 | 44.6 | 0.25x | 31.4 | 72.3 |
| dml_delete_10col_streaming | 11.0 | 46.0 | 0.24x | 30.8 | 71.2 |

このmicro benchでは `php-grpc-lite` nativeのCPU timeは `ext-grpc` より低く、実アプリ負荷試験の「CPU使用率約1.5倍」は再現していない。次の確認対象は、micro benchに含まれていない高レベルアプリ経路、並列実行時のprocess/container CPU、worker lifecycle、または負荷試験側のCPU測定単位。

## 2026-05-14 追加計測結果

### google/cloud-spanner high-level client

run id: `cpu-real-20260514-203520`

| measurement | native cpu_us/call | ext-grpc cpu_us/call | native/ext | native wall_us/call | ext-grpc wall_us/call |
|---|---:|---:|---:|---:|---:|
| small_select_1row_10col | 227.1 | 360.4 | 0.63x | 1004.7 | 1230.4 |
| dml_insert_10col | 222.0 | 343.9 | 0.65x | 1192.4 | 1257.3 |
| dml_update_10col | 206.4 | 357.6 | 0.58x | 1298.4 | 1585.3 |
| dml_delete_10col | 221.2 | 364.8 | 0.61x | 1565.8 | 1748.4 |

高レベル `google/cloud-spanner` 経路でも、process-local CPU timeではnative高CPUは再現しない。

### concurrent workers

run id: `cpu-concurrent-4w-20260514-204000`

| measurement | workers | total calls | native cpu_us/call | ext-grpc cpu_us/call | native/ext |
|---|---:|---:|---:|---:|---:|
| small_unary_100b | 4 | 20000 | 15.0 | 55.2 | 0.27x |
| small_streaming_1x100b | 4 | 20000 | 15.6 | 59.6 | 0.26x |
| small_streaming_100x100b | 4 | 20000 | 62.8 | 309.2 | 0.20x |

run id: `cpu-concurrent-8w-20260514-204000`

| measurement | workers | total calls | native cpu_us/call | ext-grpc cpu_us/call | native/ext |
|---|---:|---:|---:|---:|---:|
| small_unary_100b | 8 | 24000 | 17.6 | 83.2 | 0.21x |
| small_streaming_1x100b | 8 | 24000 | 18.3 | 91.5 | 0.20x |
| small_streaming_100x100b | 8 | 24000 | 80.0 | 401.3 | 0.20x |

複数PHP worker process同時実行でも、native高CPUは再現しない。

### client lifecycle

run id: `cpu-micro-lifecycle-20260514-204100`

| measurement | native cpu_us/call | ext-grpc cpu_us/call | native/ext |
|---|---:|---:|---:|
| small_unary_100b | 11.2 | 40.9 | 0.27x |
| new_client_unary_100b | 11.2 | 360.8 | 0.03x |
| small_streaming_1x100b | 10.9 | 43.8 | 0.25x |
| new_client_streaming_1x100b | 12.4 | 366.1 | 0.03x |

client objectをcallごとに作り直す条件でもnative高CPUは再現しない。むしろext-grpcはchannel upper bound warningを大量に出し、CPU/wallが大きく悪化する。

### php-fpm request lifecycle

`Dockerfile.fpm-ext-grpc` は `php-grpc-lite-dev-ext-grpc` のビルド済み `grpc.so` / `protobuf.so` を流用し、PECLからext-grpcを再ビルドしない。`bench/fpm-cpu-compare.sh` は `pm=static` / `pm.max_children=1` のFPM workerにFastCGI requestを投げ、worker process CPU ticksとFPM container cgroup CPUを読む。

| measurement | native worker_cpu_us/request | ext-grpc worker_cpu_us/request | native/ext | native cgroup_cpu_us/request | ext-grpc cgroup_cpu_us/request | native/ext |
|---|---:|---:|---:|---:|---:|---:|
| small_unary_100b | 110.0 | 170.0 | 0.65x | 126.4 | 201.8 | 0.63x |
| small_streaming_1x100b | 140.0 | 190.0 | 0.74x | 166.3 | 211.8 | 0.79x |

FPM request lifecycle全体に近づけても、nativeのCPU使用量がext-grpcより大きくなる現象は再現していない。worker CPUとcontainer cgroup CPUのどちらでもnativeのほうが低い。

### Laravel + laravel-spanner request lifecycle

`tools/benchmark/laravel-spanner-app` の最小Laravel fixtureで、`colopl/laravel-spanner`、`google/cloud-spanner` v1、Spanner emulatorを経由するFPM request lifecycleを追加した。測定前に同じFPM workerで `setup` と Spanner session `warmup` を実行する。

| measurement | native worker_cpu_us/request | ext-grpc worker_cpu_us/request | native/ext | native cgroup_cpu_us/request | ext-grpc cgroup_cpu_us/request | native/ext |
|---|---:|---:|---:|---:|---:|---:|
| select_1row_10col | 2750.0 | 2900.0 | 0.95x | 2839.5 | 2937.1 | 0.97x |
| dml_insert_10col | 2650.0 | 2900.0 | 0.91x | 2735.8 | 2965.7 | 0.92x |
| dml_update_10col | 3100.0 | 3300.0 | 0.94x | 3165.7 | 3413.4 | 0.93x |
| dml_delete_10col | 2900.0 | 3200.0 | 0.91x | 2954.9 | 3288.7 | 0.90x |

Laravel + laravel-spanner経路でも、現ローカルDocker環境ではnative高CPUは再現していない。

### Laravel + laravel-spanner load test

`bench/fpm-laravel-spanner-load-compare.sh` で nginx + hey を使い、client concurrency 16 / FPM worker 16 の並列負荷を追加した。測定前に `setup` と `warmup` をHTTP/FastCGI経由で実行する。

実行条件:

- requests: 1000
- client concurrency: 16
- FPM `pm=static`
- FPM workers: 16
- HTTP load tool: `hey`
- Spanner emulator

| measurement | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p90_ms | ext-grpc p90_ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| select_1row_10col | 367.6631 | 316.8997 | 6387.8 | 8963.9 | 0.71x | 42.3 | 49.6 | 85.9 | 94.5 |
| dml_insert_10col | 223.9122 | 203.4608 | 7524.5 | 10812.5 | 0.70x | 68.7 | 76.0 | 111.7 | 117.9 |
| dml_update_10col | 134.3244 | 133.8950 | 9752.8 | 13637.8 | 0.72x | 116.4 | 116.3 | 167.9 | 169.8 |
| dml_delete_10col | 136.9663 | 132.2545 | 9688.7 | 13937.4 | 0.70x | 114.5 | 118.5 | 173.9 | 173.5 |

client16 / worker16 の並列負荷でも、現ローカルDocker環境ではnative高CPUは再現していない。cgroup CPUではnativeがext-grpcの約0.70〜0.72x。

## 現時点のレビュー

- `php-grpc-lite` transport単体、`google/cloud-spanner`高レベル経路、複数PHP process並列、client object per-call lifecycleのいずれでも、process-local CPU timeでは実アプリ負荷試験の「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- php-fpm request lifecycleでも、worker CPU / container cgroup CPUのどちらでも「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- Laravel + laravel-spanner request lifecycleでも、worker CPU / container cgroup CPUのどちらでも「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- nginx + hey + client16 / FPM worker16 の並列負荷でも、container cgroup CPUでは「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- 再現しないため、現時点でC transport hot pathを最適化する根拠は弱い。
- 次に見るべき対象は、このリポジトリ内micro benchではなく、実アプリ負荷試験で使った測定単位との差分である。特に、HTTP/FPM/FrankenPHP request lifecycle全体、container全体CPU、固定RPS下のCPU%、アプリ側のSpanner/GAX client生成・認証・middleware、負荷試験ツールの集計範囲を確認する。

## 判断ログ

- 実アプリのCPU使用率差を直接再現する前段として、外部負荷ツールではなくプロセス内 `getrusage()` によるmicro benchを追加した。
- CPU差分を見るsuiteではRPCごとのspan生成を測定loopから外し、OTEL exportはaggregate summaryに限定する。
- `franken-go` backendは今回の観測対象から外す。

## 必要な入力

- 実アプリ負荷試験のRPC種類、RPS、並列数、worker数、payload size、TLS有無、Spanner/GAX有無。
- CPU使用率の測定方法と単位。
- ext-grpc / php-grpc-lite の同時比較条件。

## 完了条件

- CPU 1.5倍差分の主因が説明できる。
- 改善可能な項目と、設計上受け入れる項目が分離されている。
- 採用する改善について、主要ベンチと実アプリ条件で回帰がない。
