# google/cloud-spanner 実経路ベンチマーク 2026-05-14

## 目的

既存のSpanner shape benchmarkはGo test-serverに対する合成RPCなので、`google/cloud-spanner` の高レベルAPIから `grpc/grpc` wrapperを経由した実際の `Grpc\*Call` lifecycleを別枠で確認した。

## 条件

- suite: `spanner-real-client`
- command: `BENCH_TAG=spanner-real-client-20260514 ./bench/compare.sh spanner-real-client --calls=100 --warmup-calls=5`
- target: `spanner-emulator:9010`
- schema: 10 columns (`Id`, 2 date columns, 3 string columns, 2 int columns, bool, float)
- measured API:
  - `Database::execute()` small SELECT 1 row / 10 columns
  - `Transaction::executeUpdate()` DML insert / update / delete, each committed in its own transaction
- primary source: OTEL spans summarized by `tools/benchmark/otelop-summary.php`

## 結果

| measurement | grpc-lite native p50 | grpc-lite native p99 | ext-grpc p50 | ext-grpc p99 | 見解 |
| --- | ---: | ---: | ---: | ---: | --- |
| small SELECT 1row/10col | 2134.6µs | 3309.7µs | 4615.8µs | 9177.9µs | 実 `Database::execute()` 経路ではnativeが優位 |
| DML insert 10col | 1034.8µs | 1928.2µs | 1212.7µs | 3789.3µs | nativeがp50/p99とも優位 |
| DML update 10col | 1141.1µs | 1428.0µs | 1353.3µs | 2508.9µs | nativeがp50/p99とも優位 |
| DML delete 10col | 1279.1µs | 2388.1µs | 1363.8µs | 1530.5µs | p50はnative優位、p99はext-grpc優位 |

## 判断

- Spanner主要ユースケースの実測軸として `spanner-real-client` を通常suiteに追加する。
- このsuiteはemulator内部状態の揺れを含むため、transport単体の安定観測は従来どおりGo test-serverのshape benchmarkを使う。
- lifecycle確認としては、合成RPCでは見えないGAX transport、`ServerStream` wrapper、`Result::rows()` drain、transaction commitまで含められる。
