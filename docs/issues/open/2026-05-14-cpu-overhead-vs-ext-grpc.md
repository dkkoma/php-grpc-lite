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
- Laravel + `colopl/laravel-spanner` FPM runnerを実Cloud Spannerにも向けられるようにした。`LARAVEL_SPANNER_EMULATOR_HOST` を空にし、`LARAVEL_SPANNER_PROJECT_ID` / `LARAVEL_SPANNER_INSTANCE_ID` / `LARAVEL_SPANNER_DATABASE_ID` を指定すると、emulatorを起動せずGoogle Cloud Spanner endpointへ接続する。
- FPM containerは `/var/www/.config/gcloud` をADC参照先として使う。`vast-falcon-165704` / `bench` / `laravel-bench-db` へのstartup warmupとHTTP request疎通は確認済み。

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
- `bash -n bench/fpm-laravel-spanner-load-compare.sh`
- `bash -n bench/fpm-laravel-spanner-cpu-compare.sh`
- `BENCH_LOG_DIR=var/bench-results/log-smoke-20260514-235513 LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=4 ./bench/fpm-laravel-spanner-load-compare.sh 2 1`
- `COMPOSE_FILE=compose.yaml:/private/tmp/php-grpc-lite-fpm-workers-<workers>.yaml BENCH_LOG_DIR=var/bench-results/fpm-worker-sweep-20260514-workers<workers> LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=<workers> ./bench/fpm-laravel-spanner-load-compare.sh 1000 32`
- `COMPOSE_FILE=compose.yaml:/private/tmp/php-grpc-lite-fpm-workers-32.yaml BENCH_LOG_DIR=var/bench-results/fpm-worker32-concurrency-sweep-20260515-c<concurrency> LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=32 ./bench/fpm-laravel-spanner-load-compare.sh 1000 <concurrency>`
- `COMPOSE_FILE=compose.yaml:/private/tmp/php-grpc-lite-fpm-workers-32.yaml BENCH_VARIANTS=native BENCH_LOG_DIR=var/bench-results/fpm-worker32-native-repeat-20260515-c<concurrency>-r<run> LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=32 ./bench/fpm-laravel-spanner-load-compare.sh 1000 <concurrency>`
- `COMPOSE_FILE=compose.yaml:/private/tmp/php-grpc-lite-fpm-workers-32.yaml BENCH_LOG_DIR=var/bench-results/fpm-worker32-native-sustain-20260515-c<concurrency> LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=32 ./bench/fpm-laravel-spanner-cpu-sustain.sh <concurrency> 30s native`

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

`bench/fpm-laravel-spanner-load-compare.sh` で nginx + hey を使い、client concurrency 16 / FPM worker 16 の並列負荷を追加した。FPM container起動時に `setup` とFPM worker数分のfile session pool warmupを実行し、API requestではwarmupしない。

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

client16 / worker16 のemulator並列負荷でも、現ローカルDocker環境ではnative高CPUは再現していない。cgroup CPUではnativeがext-grpcの約0.70〜0.72x。ただしemulatorは並列transaction制約があり、実アプリ条件の再現対象としては不十分。

### Laravel + laravel-spanner load test against Cloud Spanner

実Cloud Spannerの無料枠instanceを対向先にして、同じnginx + FPM構成でclient concurrency 16 / FPM worker 16を計測した。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- requests: 1000 requested / 992 completed by `hey`
- client concurrency: 16
- FPM `pm=static`
- FPM workers: 16
- HTTP load tool: `hey`
- startup: FPM container起動時にDB/table setup + `created_sessions=16`

実行コマンド:

```sh
LARAVEL_SPANNER_EMULATOR_HOST= \
LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 \
LARAVEL_SPANNER_INSTANCE_ID=bench \
LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db \
LARAVEL_SPANNER_MIN_SESSIONS=16 \
./bench/fpm-laravel-spanner-load-compare.sh 1000 16
```

| measurement | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p50_ms | ext-grpc p50_ms | native p90_ms | ext-grpc p90_ms | native max_ms | ext-grpc max_ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| select_1row_10col | 320.7352 | 335.1194 | 8923.5 | 8245.7 | 1.08x | 48.1 | 46.5 | 44.4 | 44.1 | 53.8 | 52.0 | 164.8 | 146.2 |

実Cloud Spanner + Laravel + `colopl/laravel-spanner` + FPM16/client16のSELECTでは、nativeのcgroup CPU/requestがext-grpcの約1.08xになった。実アプリで観測された1.5xほどではないが、emulatorやmicro benchでは見えなかった「native側CPUが高い」方向の差は再現した。

### Laravel + laravel-spanner load test against Cloud Spanner with 4 CPU quota

FPM application containerに `cpus: 4.0` を設定し、同じCloud Spanner instanceに対してclient concurrencyを増やした。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- requested: 1000
- FPM `pm=static`
- FPM workers: 16
- FPM container CPU quota: 4.0
- HTTP load tool: `hey`
- startup: FPM container起動時にDB/table setup + `created_sessions=16`

| concurrency | completed | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p50_ms | ext-grpc p50_ms | native p90_ms | ext-grpc p90_ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 992 | 302.4925 | 320.7258 | 8484.2 | 8757.5 | 0.97x | 51.2 | 48.6 | 46.9 | 46.2 | 54.2 | 52.5 |
| 32 | 992 | 314.0246 | 334.0428 | 8844.7 | 7943.2 | 1.11x | 99.8 | 94.2 | 94.8 | 92.1 | 110.0 | 99.3 |
| 64 | 960 | 306.4898 | 322.4144 | 9268.5 | 8494.7 | 1.09x | 202.2 | 192.2 | 194.4 | 190.5 | 234.0 | 201.8 |

CPU 4.0制限下では、concurrency 32/64でnativeのCPU/requestがext-grpc比で約1.09〜1.11xになった。client concurrencyを増やしても、今回のローカル+Cloud Spanner条件では1.5xまでは広がらない。latencyはnativeが一貫して小幅に悪く、特にp90差はconcurrency 64で約32msまで広がる。

### Laravel + laravel-spanner load test against Cloud Spanner by FPM worker count

FPM application containerのCPU quotaを4.0に固定し、client concurrency 32でFPM worker数とstartup warmup session数を 4 / 8 / 16 / 32 に変えて計測した。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- requested: 1000
- completed: 992
- client concurrency: 32
- FPM `pm=static`
- FPM container CPU quota: 4.0
- HTTP load tool: `hey`
- startup: FPM container起動時にDB/table setup + `created_sessions=<workers>`
- logs: `var/bench-results/fpm-worker-sweep-20260514-workers{4,8,16,32}/`

| workers | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p50_ms | ext-grpc p50_ms | native p90_ms | ext-grpc p90_ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 87.5278 | 93.9674 | 7078.8 | 6422.9 | 1.10x | 359.6 | 335.0 | 354.3 | 338.6 | 370.7 | 349.6 |
| 8 | 175.4927 | 182.5776 | 7578.4 | 6807.4 | 1.11x | 179.6 | 172.3 | 175.3 | 170.7 | 186.7 | 180.5 |
| 16 | 317.6916 | 346.6311 | 8428.0 | 8195.5 | 1.03x | 97.8 | 91.0 | 93.1 | 88.6 | 105.6 | 96.4 |
| 32 | 350.7956 | 341.2979 | 11182.7 | 11347.0 | 0.99x | 87.6 | 89.5 | 81.3 | 83.3 | 120.3 | 120.5 |

worker数を増やすと、絶対値としてはnative/ext-grpcともにCPU/requestが増える傾向がある。特に16→32では両実装とも約1.35x程度まで増え、worker過多やFPM/process schedulingの固定費は疑わしい。一方でnative/ext-grpc比は 4/8 workersで約1.10〜1.11x、16 workersで約1.03x、32 workersで約0.99xであり、worker数増加がnative固有の1.5x差を単調に広げる結果にはなっていない。

この結果から、worker数が多いほど競合で悪くなる可能性は「両実装共通のCPU/request増」としては残るが、現条件では `php-grpc-lite` 固有のCPU 1.5x差の主因とは言い切れない。次は、実アプリ負荷試験と同じFPM worker数、client並列、固定RPS、Spanner region、Laravel middleware構成で再現性を詰める必要がある。

### Laravel + laravel-spanner load test against Cloud Spanner by client concurrency

FPM application containerのCPU quotaを4.0、FPM worker数を32、startup warmup session数を32に固定し、client concurrencyを32刻みで増やした。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- requested: 1000
- FPM `pm=static`
- FPM workers: 32
- FPM container CPU quota: 4.0
- HTTP load tool: `hey`
- startup: FPM container起動時にDB/table setup + `created_sessions=32`
- logs: `var/bench-results/fpm-worker32-concurrency-sweep-20260515-c{32,64,96,128,160,192}/`

| concurrency | completed | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p50_ms | ext-grpc p50_ms | native p90_ms | ext-grpc p90_ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 992 | 282.8727 | 367.6624 | 13788.8 | 10529.8 | 1.31x | 108.2 | 82.5 | 96.3 | 75.4 | 156.3 | 102.2 |
| 64 | 960 | 342.2630 | 241.0325 | 11628.8 | 16480.0 | 0.71x | 181.2 | 257.6 | 170.5 | 233.9 | 224.4 | 374.7 |
| 96 | 960 | 330.8051 | 353.5712 | 12060.7 | 11418.0 | 1.06x | 279.9 | 261.0 | 242.8 | 238.9 | 381.2 | 371.8 |
| 128 | 896 | 307.6711 | 356.6245 | 12819.3 | 11336.7 | 1.13x | 391.7 | 334.4 | 349.4 | 327.0 | 531.0 | 376.1 |
| 160 | 960 | 350.1914 | 365.4499 | 10996.4 | 10957.4 | 1.00x | 425.7 | 407.3 | 415.1 | 398.6 | 493.8 | 497.9 |
| 192 | 960 | 358.0936 | 368.8266 | 11074.6 | 10773.5 | 1.03x | 489.4 | 478.1 | 499.9 | 478.2 | 558.2 | 532.7 |

concurrency 32ではnativeがext-grpc比でCPU/request 1.31x、latencyも悪い。ただし64ではext-grpc側だけ大きく悪化し、96以降はnative/ext-grpc比が約1.00〜1.13xに戻る。単純にclient並列を増やすほどnative固有のCPU差が拡大する、という形ではない。

concurrency 32の1.31xは実アプリ観測値1.5xに近づくが、この単発結果だけではSpanner側の揺れ、warm connection/session状態、container recreate順序、Cloud Spannerの一時的な遅延差を除外できない。採用すべき次の計測は、同一条件を複数回繰り返して中央値/外れ値を見たうえで、固定RPSでCPU消費を比較すること。

### Laravel + laravel-spanner native repeat at worker32

`bench/fpm-laravel-spanner-load-compare.sh` に `BENCH_VARIANTS` を追加し、`native` のみ反復計測できるようにした。FPM worker32 / CPU 4.0 / startup warmup session32で、client concurrency 32 と 64 を確認した。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- requested: 1000
- FPM `pm=static`
- FPM workers: 32
- FPM container CPU quota: 4.0
- variants: `native`
- HTTP load tool: `hey`
- logs: `var/bench-results/fpm-worker32-native-repeat-20260515-c{32,64}-r*/`

| concurrency | run | completed | rps | cpu_us/request | avg_ms | p50_ms | p90_ms | max_ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 1 | 992 | 329.7756 | 10706.0 | 93.3 | 77.1 | 148.6 | 338.7 |
| 32 | 2 | 992 | 320.7137 | 10906.4 | 97.2 | 80.6 | 161.9 | 337.2 |
| 32 | 3 | 992 | 333.9769 | 10842.6 | 92.2 | 79.1 | 151.3 | 241.7 |
| 32 | 4 | 992 | 328.0015 | 10223.1 | 92.0 | 74.6 | 151.5 | 388.5 |
| 32 | 5 | 992 | 305.6451 | 10619.7 | 97.7 | 77.6 | 163.1 | 389.1 |
| 64 | 1 | 959 | 46.6135 | 10185.0 | 301.8 | 138.5 | 177.4 | 6470.5 |
| 64 | 2 | 960 | 220.2776 | 18159.0 | 279.2 | 279.9 | 379.2 | 560.9 |
| 64 | 3 | 960 | 201.6061 | 19610.0 | 304.7 | 308.9 | 397.3 | 534.3 |

| concurrency | valid runs | median rps | median cpu_us/request | median avg_ms | median p50_ms | median p90_ms | note |
|---:|---:|---:|---:|---:|---:|---:|---|
| 32 | 5 | 328.0015 | 10706.0 | 93.3 | 77.6 | 151.5 | 安定 |
| 64 | 3 | 201.6061 | 18159.0 | 301.8 | 279.9 | 379.2 | 大きく不安定。4回目は `docker compose up --force-recreate` が停止したため中断 |

native単体で見ると、concurrency 32はかなり安定している。concurrency 64は同じ32 worker / 4 CPU条件でも極端に不安定で、rps低下、tail悪化、Docker compose停止が発生した。64並列はSpanner/API側の揺れだけでなく、ローカルDocker/FPM再作成を含む計測基盤の安定性にも疑いがある。

この結果から、まず安定して比較しやすいのは `4 CPUs / 32 workers / client concurrency 32`。ここでnative/ext-grpcを複数回比較して中央値を見るのが次の妥当なステップ。64並列は飽和・不安定系の観測として残すが、CPU差分の主判定には使いにくい。

### Laravel + laravel-spanner native sustained CPU sampling

比較値より高負荷時のCPU状態を分析するため、`bench/fpm-laravel-spanner-cpu-sustain.sh` を追加した。`hey -z` で固定時間loadを流しながら、FPM containerの `/sys/fs/cgroup/cpu.stat`、FPM worker aggregate ticks、context switchを時系列で `fpm-cpu-samples.csv` に保存する。

実行条件:

- project: `vast-falcon-165704`
- instance: `bench`
- database: `laravel-bench-db`
- duration: 30s
- FPM `pm=static`
- FPM workers: 32
- FPM container CPU quota: 4.0
- variants: `native`
- HTTP load tool: `hey -z`
- sample interval: 1s

#### c32

- log: `var/bench-results/fpm-worker32-native-sustain-20260515-c32/`
- requests: 3974 responses / 30.1273s
- rps: 131.9067
- average latency: 242.1ms
- p50: 202.5ms
- p90: 343.3ms
- p99: 826.2ms
- slowest: 1432.9ms

FPM cgroup CPU summary:

| samples | duration_s | cpu_cores_avg | cpu_cores_min | cpu_cores_max | throttled_periods | throttled_usec | worker_ticks | voluntary_ctxt | nonvoluntary_ctxt |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 19 | 31 | 3.986 | 1.360 | 7.050 | 244 | 39630863 | 10898 | 39631 | 135523 |

4 CPU quotaに対して平均 `3.986 cores` まで使い切っており、31秒の観測中に `throttled_usec=39.6s` が発生している。`nr_throttled` も大きく、nonvoluntary context switchも多い。つまりc32時点でFPM containerはCPU quota上限に張り付いており、latency tailはgRPC transport単体というより、FPM worker過多 + CPU quota throttling + Cloud Spanner待ちが混ざる状態になっている。

この状態でCPU差分を見る場合、単純なrequest完了数あたりCPUだけだと「どちらが先にthrottleされるか」「Cloud Spanner待ち中にworkerが詰まるか」の影響が大きい。次の観測は、同じc32でnative/ext-grpcそれぞれの sustained CPU sampling を取り、`cpu_cores_avg`、`throttled_usec`、`nonvoluntary_ctxt`、p99 latencyを比較するのがよい。

#### c64

- log: `var/bench-results/fpm-worker32-native-sustain-20260515-c64/`
- 結果: 無効

c64は `docker compose run --rm loadgen ... -z 30s -c 64` の起動が停止し、`hey` 出力が生成されなかった。前段の反復計測でもc64はrps/tailが大きく崩れていたため、現ローカルDocker環境では主判定条件にしない。飽和時の異常系観測としては残すが、まずc32を安定条件として分析する。

## 対応候補

高負荷時CPUの観測から、現時点で優先すべき対応候補は以下。

1. **固定RPS + sustained sampling**: 最大負荷ではなく同じRPSを与え、native/ext-grpcの `cpu_cores_avg` / `throttled_usec` / p99を比較する。CPU quotaに張り付く条件では、実装差とscheduler差が混ざりやすい。
2. **FPM worker数チューニング**: 4 CPU quotaに32 workerはoversubscribeが強い。worker 8/16/32で sustained sampling を取り、throughputとthrottlingの折り合いを見る。
3. **CPU profile取得**: c32条件でFPM workerのPHP/C CPU hot pathを採取する。候補は `perf` / `phpspy` / `callgrind`。まずローカルDockerで使える手段を確認する。
4. **Laravel/Spanner経路分解**: transport単体microではnative高CPUが出ていないため、`google/cloud-spanner` / `colopl/laravel-spanner` / protobuf / Laravel request lifecycle のどこでCPUが増えるか分ける。

## 現時点のレビュー

- `php-grpc-lite` transport単体、`google/cloud-spanner`高レベル経路、複数PHP process並列、client object per-call lifecycleのいずれでも、process-local CPU timeでは実アプリ負荷試験の「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- php-fpm request lifecycleでも、worker CPU / container cgroup CPUのどちらでも「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- Laravel + laravel-spanner request lifecycleでも、worker CPU / container cgroup CPUのどちらでも「nativeがext-grpc比でCPU約1.5倍」は再現しなかった。
- nginx + hey + client16 / FPM worker16 のemulator並列負荷では、container cgroup CPUでnative高CPUは再現しなかった。
- 実Cloud Spannerを対向先にしたLaravel + `colopl/laravel-spanner`経路では、nativeがext-grpc比で約1.08〜1.11xのCPU/requestとなり、差の方向は再現した。
- CPU 4.0制限 + client concurrency 16/32/64でも1.5xには届いていない。FPM worker数を4/8/16/32に変えると両実装ともworker過多でCPU/requestが増えるが、native固有差は単調に広がらない。
- FPM worker32 / CPU 4.0でclient concurrencyを32刻みに増やすと、concurrency 32ではnative/ext-grpc CPU 1.31xが出たが、64以降は単調増加せず約0.71〜1.13xに散る。次に見るべき対象は、同一条件の複数回繰り返し、固定RPS下のCPU%、HTTP/FPM/FrankenPHP request lifecycle全体、アプリ側middleware、session warmupの実アプリとの差、Spanner instance/regionと負荷試験環境の距離である。
- native単体反復では、FPM worker32 / CPU 4.0 / client concurrency 32が安定し、median `10706.0 cpu_us/request`。concurrency 64はtailとrpsが大きく崩れ、計測基盤側の停止も発生したため、主判定条件としてはconcurrency 32を優先する。
- sustained CPU samplingでは、native c32時点でFPM containerが4 CPU quotaを平均 `3.986 cores` まで使い切り、`throttled_usec=39.6s` / `nonvoluntary_ctxt=135523` が発生した。高負荷分析はc32を主条件にし、固定RPS、worker数、profilingへ進める。

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
