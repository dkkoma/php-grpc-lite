# franken-go channel pool benchmark

Status: Closed
Date: 2026-05-13

## 目的

`github.com/dkkoma/frankenphp-grpc-go-client` の channel pool 実装後の `franken-go` backend 性能を、main の主要ベンチマークと比較する。

## 背景

以前の franken-go backend は channel pool がなく、RPCごとの固定費が大きい可能性があった。frankenphp-grpc-go-client 側に channel pool が実装されたため、php-grpc-lite main の native backend、公式 ext-grpc、franken-go backend の代表ケースを同条件で再計測した。

## スコープ

- `dev-franken-grpc-go` image を最新の frankenphp-grpc-go-client ref で再ビルドする。
- Spanner代表形状: `spanner-dml-unary-shape`、`small-select-streaming` を franken-go 含みで計測する。
- 主要throughput系のspot checkを追加する。
- 結果を既存の主要ベンチマークと比較して記録する。

## 非スコープ

- frankenphp-grpc-go-client 側のコード変更。
- php-grpc-lite の backend selection や transport 実装変更。

## 計画

1. 作業状態とベンチ導線を確認する。Done.
2. `dev-franken-grpc-go` を最新refで再ビルドする。Done.
3. Spanner代表形状を `native` / `franken-go` / `ext-grpc` で計測する。Done.
4. 主要suiteの追加比較を必要分実行する。Done.
5. 結果と判断を記録する。Done.

## 進捗

- `dev-franken-grpc-go` は `frankenphp-grpc-go-client@main` を `v0.0.0-20260513131237-cace7741d35d` として取得してビルドした。
- `tools/test/check-franken-go-backend.sh` は成功。
- OTEL summaryが `php-grpc-lite native` と `php-grpc-lite franken-go` を同じ `implementation` に畳んでいたため、`benchmark.transport` をgrouping/displayへ追加した。
- 結果は `docs/benchmarks/franken-go-channel-pool-2026-05-13.md` に記録した。

## 検証

- `docker compose build --no-cache dev-franken-grpc-go`
- `docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh`
- `docker compose run --rm dev php -l tools/benchmark/otelop-summary.php`
- `BENCH_TAG=franken-pool-dml-20260513-132200 BENCH_OTEL_RUN_ID=franken-pool-dml-20260513-132200 INCLUDE_FRANKEN=1 DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 BENCH_OTEL_SUMMARY_LIMIT=20000 ./bench/compare-spanner-dml-unary-shape.sh`
- `BENCH_TAG=franken-pool-small-select-20260513-132300 BENCH_OTEL_RUN_ID=franken-pool-small-select-20260513-132300 INCLUDE_FRANKEN=1 WARMUP_STREAMS=10 BENCH_OTEL_SUMMARY_LIMIT=20000 ./bench/compare-small-select-streaming.sh`
- `BENCH_OTEL_RUN_ID=franken-pool-throughput-short-20260513-132500` で `throughput-unary` spot check。
- `BENCH_OTEL_RUN_ID=franken-pool-stream-throughput-20260513-132600` で `throughput-streaming` spot check。

## 判断ログ

- franken-go は channel pool後に ext-grpc 近辺の固定費まで改善している可能性がある。
- Spanner主用途の small unary / small server streaming では main native backend のp50優位が維持されており、default backendをfranken-goに切り替える根拠はない。
- franken-go backend は FrankenPHP環境で grpc-go channel semantics を使いたい場合のoptional backendとして残す。
- `throughput-unary --duration=3` はOTEL span全件保持でmemory limitに到達したため、長時間throughput計測はchunk flushなどの追加整備が必要。

## 完了条件

- channel pool 実装後の franken-go backend の代表ベンチ結果がある。Done.
- native / ext-grpc / franken-go の差分と見解が記録されている。Done.
