# franken-go lightweight UnaryCall constructor benchmark

Status: Closed
Date: 2026-05-14

## 目的

`frankenphp-grpc-go-client` の `FrankenGrpc\UnaryCall::__construct()` 軽量化後の `franken-go` backend 性能を、直近の channel pool 計測と比較する。

## 背景

2026-05-13 の計測では channel pool 後の franken-go backend を確認した。その後、`FrankenGrpc\UnaryCall::__construct()` 軽量化が upstream main に入ったため、main の unary代表ベンチマークで影響を確認した。

## スコープ

- `dev-franken-grpc-go` を upstream main で再ビルドする。
- `spanner-dml-unary-shape` を franken-go 含みで再計測する。
- `throughput-unary` をspot checkとして再計測する。
- 2026-05-13 channel pool計測と比較する。

## 非スコープ

- php-grpc-lite transport実装変更。
- frankenphp-grpc-go-client側の変更。
- `FrankenGrpc\UnaryCall` object生成だけのmicro benchmark追加。

## 進捗

- `dev-franken-grpc-go` は `frankenphp-grpc-go-client@main` を `v0.0.0-20260513215327-f290d0169347` として取得してビルドした。
- `tools/test/check-franken-go-backend.sh` は成功。
- DML unaryとthroughput-unary spot checkを実行した。
- 結果は `docs/benchmarks/franken-go-unary-call-constructor-2026-05-14.md` に記録した。

## 検証

- `docker compose build --no-cache dev-franken-grpc-go`
- `docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh`
- `BENCH_TAG=franken-unary-ctor-dml-20260514-070000 BENCH_OTEL_RUN_ID=franken-unary-ctor-dml-20260514-070000 INCLUDE_FRANKEN=1 DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 BENCH_OTEL_SUMMARY_LIMIT=20000 ./bench/compare-spanner-dml-unary-shape.sh`
- `BENCH_TAG=franken-unary-ctor-throughput-20260514-070100 BENCH_OTEL_RUN_ID=franken-unary-ctor-throughput-20260514-070100 ./bench/run.sh throughput-unary --duration=0.2 --payload-bytes=100` 相当の native / ext-grpc / franken-go spot check。

## 判断ログ

- `FrankenGrpc\UnaryCall` は franken-go unary pathでRPCごとに生成されるため、warm channelでもRPC固定費として現れる。
- ただし今回のRPC全体spanでは、一貫した改善は確認できなかった。
- throughput-unaryは前回 franken-go `61.0 / 385.1us`、今回 `62.7 / 391.9us` でほぼ同等。
- Spanner DML unaryは一部p50が小幅改善したが、begin_txnやp99は悪化しており、ノイズを超えた改善とは判断しない。
- default backendは引き続き native が妥当。

## 完了条件

- 最新 franken-go main で代表ベンチ結果がある。Done.
- 2026-05-13 channel pool計測との差分と、constructor軽量化の見え方が記録されている。Done.
