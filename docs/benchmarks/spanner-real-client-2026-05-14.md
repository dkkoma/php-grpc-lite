# google/cloud-spanner 実経路ベンチマーク 2026-05-14

## 目的

既存のSpanner shape benchmarkはGo test-serverに対する合成RPCなので、`google/cloud-spanner` の高レベルAPIから `grpc/grpc` wrapperを経由した実際の `Grpc\*Call` lifecycleを別枠で確認した。

## 条件

- suite: `spanner-real-client`
- command: `BENCH_TAG=spanner-real-client-tx-scope-20260514 ./bench/compare.sh spanner-real-client --calls=100 --warmup-calls=5`
- target: `spanner-emulator:9010`
- schema: 10 columns (`Id`, 2 date columns, 3 string columns, 2 int columns, bool, float)
- measured API:
  - `Transaction::execute()` small SELECT 1 row / 10 columns
  - `Transaction::executeUpdate()` DML insert / update / delete
- transaction scope: read-write transaction は計測前に開始し、rollbackは計測外で実行する
- primary source: OTEL spans summarized by `tools/benchmark/otelop-summary.php`

## 結果

| measurement | grpc-lite native p50 | grpc-lite native p99 | ext-grpc p50 | ext-grpc p99 | 見解 |
| --- | ---: | ---: | ---: | ---: | --- |
| small SELECT 1row/10col | 778.6µs | 951.4µs | 844.7µs | 979.2µs | 開始済みtransaction内のSELECTではnativeが小幅優位 |
| DML insert 10col | 788.0µs | 918.1µs | 816.0µs | 889.2µs | p50はnative優位、p99はext-grpcが小幅優位 |
| DML update 10col | 930.9µs | 1080.5µs | 968.2µs | 1199.5µs | nativeがp50/p99とも優位 |
| DML delete 10col | 1059.2µs | 1217.0µs | 1123.9µs | 1262.6µs | nativeがp50/p99とも優位 |

## 判断

- Spanner主要ユースケースの実測軸として `spanner-real-client` を通常suiteに追加する。
- このsuiteはemulator内部状態の揺れを含むため、transport単体の安定観測はGo test-serverの汎用benchmarkを使う。
- lifecycle確認としては、合成RPCでは見えないGAX transport、`ServerStream` wrapper、`Result::rows()` drain、transaction内statement実行を含められる。
