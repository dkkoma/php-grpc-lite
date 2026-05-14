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
