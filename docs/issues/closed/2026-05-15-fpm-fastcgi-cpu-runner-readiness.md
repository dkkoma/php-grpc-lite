---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# FastCGI CPU runner のFPM ready待ちを追加する

## 目的

mixed transaction actionでnative単体CPU計測を行う際、`bench/fpm-laravel-spanner-cpu-compare.sh` がFPM起動直後にFastCGI接続して失敗する問題を修正する。

## 背景

`transaction_select2_update1_insert1` を既定actionにした後、FastCGI CPU runnerを実行すると `Could not connect to fpm-lifecycle-16:9000` で失敗した。runnerが `docker compose up -d --force-recreate` の直後にready確認なしで `cgi-fcgi` を呼んでいたため、FPM processがlisten開始する前に接続していた。

## スコープ

- `bench/fpm-laravel-spanner-cpu-compare.sh` にFPM ready待ちを追加する。
- 修正後にmixed transaction actionでnative/ext-grpcのFastCGI CPU計測を実行する。

## 非スコープ

- FPM service自体の起動方式変更。
- production runtimeの変更。

## 進捗

- `wait_until_fpm_ready()` を追加し、計測前に対象actionでFastCGI smokeを最大60秒リトライするようにした。
- ready待ち後に従来のwarmup 1 requestを実行する流れは維持した。

## 検証

- `bash -n bench/fpm-laravel-spanner-cpu-compare.sh`
- `BENCH_LOG_DIR=var/bench-results/fpm-mixed-cpu-compare-20260515-r2 LARAVEL_SPANNER_EMULATOR_HOST= LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 LARAVEL_SPANNER_INSTANCE_ID=bench LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db LARAVEL_SPANNER_MIN_SESSIONS=32 ./bench/fpm-laravel-spanner-cpu-compare.sh 20`

結果:

| variant | worker CPU/request | cgroup CPU/request |
| --- | ---: | ---: |
| native | 16,000.0µs | 24,429.2µs |
| ext-grpc | 12,500.0µs | 21,459.5µs |

## 判断ログ

- 問題はFPM起動直後の計測runner不安定性であり、native transport本体の不具合ではない。
- mixed transactionのCloud Spanner負荷ではCPUは飽和せず、Spanner write transaction待ちが支配的だった。
- 固定件数比較ではnativeがext-grpcよりCPU/requestで約12%高いが、controlled microではnativeが軽いため、現時点ではtransport単体hot pathとは断定しない。

## 完了条件

- FastCGI CPU runnerがFPM起動直後でもready待ちして計測へ進める。
- mixed transaction actionでnative/ext-grpcのCPU比較を取得できる。

## Fix summary

`bench/fpm-laravel-spanner-cpu-compare.sh` にFPM ready待ちを追加し、起動直後のFastCGI接続失敗を解消した。

## Fix commit

- CPU調査 Step 6: FastCGI CPU runnerのready待ちを追加
