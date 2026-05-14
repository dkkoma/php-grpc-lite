---
Status: Closed
Owner: Codex
Created: 2026-05-14
Branch: main
---

# Laravel + laravel-spanner 経路のCPU計測環境

## 目的

実アプリ負荷試験で観測された `php-grpc-lite` のCPU使用率悪化を、このリポジトリ内で再現・切り分けできるようにする。

## 背景

既存の `cpu-micro`、`cpu-spanner-real-client`、FPM request lifecycle計測では、`php-grpc-lite` が `ext-grpc` よりCPUを多く使う現象は再現していない。実アプリは Laravel と `colopl/laravel-spanner` を経由しており、Spanner session はFPM worker数分をhealth check前にwarmupしている。

## スコープ

- 最小Laravel applicationをbench fixtureとして追加する。
- `colopl/laravel-spanner` 経由で Spanner emulator に接続する。
- FPM worker内で setup / session warmup / request計測を実行する。
- `php-grpc-lite` と `ext-grpc` を同じFastCGI request lifecycleで比較する。
- FPM worker CPU ticks と container cgroup CPU を取得する。

## 非スコープ

- 実アプリ全体の再現。
- nginx / load balancer / HTTP clientを含むE2E負荷試験。
- franken-go backend。
- Spanner session pool実装の変更。

## 計画

1. 最小Laravel appの依存定義を追加する。
2. Laravel bootstrappingとSpanner connection設定を追加する。
3. setup / warmup / select / DML のFastCGI entrypointを追加する。
4. native / ext-grpc のFPM worker CPU比較runnerを追加する。
5. Docker上でsmokeし、観測結果をこのissueに記録する。

## 進捗

- `tools/benchmark/laravel-spanner-app` に最小Laravel appの依存定義を追加した。
- `tools/benchmark/laravel-spanner-request.php` にFPM request entrypointを追加した。
- `bench/fpm-laravel-spanner-cpu-compare.sh` に native / ext-grpc 比較runnerを追加した。
- runnerは `setup`、`warmup` をFPM worker内で実行してから、測定対象requestのworker CPU ticksとcgroup CPUを読む。
- `bench/fpm-laravel-spanner-load-compare.sh` に nginx + hey の並列負荷runnerを追加した。
- worker16用の `fpm-lifecycle-16` / `fpm-ext-grpc-16` と、nginx proxy / loadgen serviceを追加した。

## 検証

- `docker compose run --rm dev composer --working-dir=/workspace/tools/benchmark/laravel-spanner-app install --no-interaction --prefer-dist`
- `docker compose run --rm dev php -l tools/benchmark/laravel-spanner-request.php`
- `./bench/fpm-laravel-spanner-cpu-compare.sh 1`
- `./bench/fpm-laravel-spanner-cpu-compare.sh 50`
- `./bench/fpm-laravel-spanner-cpu-compare.sh 200`
- `docker compose build fpm-lifecycle-16 fpm-ext-grpc-16`
- `docker compose build loadgen`
- `./bench/fpm-laravel-spanner-load-compare.sh 16 4`
- `./bench/fpm-laravel-spanner-load-compare.sh 1000 16`

### 2026-05-14 計測結果

実行条件:

- `php:8.4-fpm-trixie`
- `pm=static`
- `pm.max_children=1`
- Spanner emulator
- `colopl/laravel-spanner` `dev-master 01e6227`
- `google/cloud-spanner` `v1.104.1`
- `grpc/grpc` `1.80.0`
- 計測前に同一FPM workerで `setup` と `warmup` を実行

200 request:

| measurement | native worker_cpu_us/request | ext-grpc worker_cpu_us/request | native/ext | native cgroup_cpu_us/request | ext-grpc cgroup_cpu_us/request | native/ext |
|---|---:|---:|---:|---:|---:|---:|
| select_1row_10col | 2750.0 | 2900.0 | 0.95x | 2839.5 | 2937.1 | 0.97x |
| dml_insert_10col | 2650.0 | 2900.0 | 0.91x | 2735.8 | 2965.7 | 0.92x |
| dml_update_10col | 3100.0 | 3300.0 | 0.94x | 3165.7 | 3413.4 | 0.93x |
| dml_delete_10col | 2900.0 | 3200.0 | 0.91x | 2954.9 | 3288.7 | 0.90x |

Laravel + laravel-spanner 経路まで含めても、現ローカルDocker環境では `php-grpc-lite` のCPU使用量が `ext-grpc` より大きくなる現象は再現していない。

### 2026-05-14 並列負荷計測結果

実行条件:

- nginx + php-fpm
- `hey`
- requests: 1000
- client concurrency: 16
- FPM workers: 16
- Spanner emulator
- 計測前にHTTP/FastCGI経由で `setup` と `warmup` を実行

| measurement | native rps | ext-grpc rps | native cpu_us/request | ext-grpc cpu_us/request | native/ext CPU | native avg_ms | ext-grpc avg_ms | native p90_ms | ext-grpc p90_ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| select_1row_10col | 367.6631 | 316.8997 | 6387.8 | 8963.9 | 0.71x | 42.3 | 49.6 | 85.9 | 94.5 |
| dml_insert_10col | 223.9122 | 203.4608 | 7524.5 | 10812.5 | 0.70x | 68.7 | 76.0 | 111.7 | 117.9 |
| dml_update_10col | 134.3244 | 133.8950 | 9752.8 | 13637.8 | 0.72x | 116.4 | 116.3 | 167.9 | 169.8 |
| dml_delete_10col | 136.9663 | 132.2545 | 9688.7 | 13937.4 | 0.70x | 114.5 | 118.5 | 173.9 | 173.5 |

client16 / worker16 の並列負荷でも、現ローカルDocker環境では `php-grpc-lite` のCPU使用量が `ext-grpc` より大きくなる現象は再現していない。

## 判断ログ

- root `composer.json` にはLaravel依存を入れず、bench fixture appとして分離する。
- `colopl/laravel-spanner` はfixture app側の `composer.lock` に固定し、CIでも同じ依存を取得できるようにする。調査時は `_research/laravel-spanner` を参照したが、fixture app自体はpath repositoryに依存しない。
- Spanner session warmupとgRPC channel warmupを同じFPM worker内で行うため、測定前にFastCGI経由で `setup` / `warmup` actionを実行する。

## 完了条件

- [x] Docker compose上でLaravel + laravel-spanner経由のFPM requestが実行できる。
- [x] session warmup後に `select_1row_10col` とDML系を計測できる。
- [x] native / ext-grpc のworker CPUとcgroup CPUを比較できる。
