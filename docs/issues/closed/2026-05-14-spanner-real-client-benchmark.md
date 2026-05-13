---
Status: Closed
Owner: Codex
Created: 2026-05-14
---

# google/cloud-spanner 実経路ベンチマーク

## 目的

`google/cloud-spanner` の高レベルAPIから `grpc/grpc` wrapper を経由して呼ばれる実際の `Grpc\*Call` lifecycleを、主要ベンチマークとして計測できるようにする。

## 背景

既存の `spanner-dml-unary-shape` と `small-select-streaming` は Go test-server に対する合成RPCで、Spannerのprotobuf payload shapeや小さいserver streamingに近い形状を安定観測するためのものだった。一方で `google/cloud-spanner` の `Database::execute()` や `Transaction::executeUpdate()` はGAX transport、call wrapper、Result drain、transaction commitを含むため、Grpc系オブジェクトの実利用時 lifecycle を別途確認する必要がある。

## スコープ

- Spanner emulator を相手に、`Google\Cloud\Spanner\SpannerClient` の高レベルAPIを使うベンチsuiteを追加する。
- small SELECT 1 row / 10 column と DML insert / update / delete を主要ケースにする。
- `php-grpc-lite` と公式 `ext-grpc` を同じrunnerで比較できるようにする。
- 結果はOTEL spanを一次ソースにして `docs/benchmarks/` に記録する。

## 非スコープ

- Spanner emulator の内部性能評価。
- synthetic Go test-server shape benchmark の置き換え。
- franken-go backend の必須比較。

## 計画

1. 高レベルSpanner APIを使うbenchmark scriptを追加する。
2. `bench/run.sh` / `bench/compare.sh` から実行できるsuiteとして登録する。
3. Docker compose上で `php-grpc-lite` と公式 `ext-grpc` を比較計測する。
4. 結果と判断を `docs/benchmarks/` に記録する。

## 進捗

- `tools/benchmark/spanner-real-client.php` を追加した。
- `bench/run.sh` に `spanner-real-client` suiteを登録した。
- `tools/benchmark/otelop-summary.php` で `benchmark.operation_shape` をshape keyとして表示できるようにした。
- `docs/benchmarks/spanner-real-client-2026-05-14.md` に計測結果を記録した。

## 検証

- `docker compose run --rm dev php -l tools/benchmark/spanner-real-client.php`
- `bash -n bench/run.sh && bash -n bench/compare.sh`
- `BENCH_TAG=spanner-real-smoke-20260514071211 ./bench/compare.sh spanner-real-client --calls=2 --warmup-calls=1`
- `BENCH_TAG=spanner-real-client-20260514 ./bench/compare.sh spanner-real-client --calls=100 --warmup-calls=5`

## 判断ログ

- setup/teardownは計測対象外なので、既存のSpanner admin fixtureを使ってよい。
- 計測対象は必ず `Google\Cloud\Spanner\SpannerClient` から取得した `Database` / `Transaction` の高レベルAPIに限定する。
- `Transaction::executeUpdate()` は実際には `ExecuteStreamingSql` と `Commit` を含む高レベルDML lifecycleとして計測する。

## 完了条件

- `./bench/compare.sh spanner-real-client` で比較できる。
- small SELECT / DML insert / DML update / DML delete のspanがOTELに出る。
- 計測結果がdocsに残っている。

## Fix summary

`google/cloud-spanner` の高レベルAPI経由でSpanner emulatorに接続し、small SELECT 1row/10col と DML insert/update/delete 10colを測る主要suiteを追加した。

## Fix commit

- 未コミット
