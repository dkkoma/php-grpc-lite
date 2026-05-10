# Benchmark Guide

このディレクトリはベンチ結果の記録を置く。現在のベンチ実行入口は `bench/` 配下の専用runnerに集約している。

旧ベンチrunner、aggregate parse、regression baseline運用は廃止済み。CIでも使っていないため、通常の性能確認には使わない。

## 通常の比較線

通常の継続比較は **php-grpc-lite vs 公式 ext-grpc** に固定する。ベンチ結果の一次ソースは `otelop` にexportしたOTEL spanで、JSON/TSV保存やregression baselineは使わない。

```bash
./bench/compare-spanner-dml-unary-shape.sh
./bench/compare-small-select-streaming.sh
./bench/compare.sh throughput-unary --duration=3
./bench/compare.sh rtt-unary --calls=20
```

`BENCH_TAG` または `BENCH_OTEL_RUN_ID` でrun idを固定できる。runnerはcompose内の `otelop` を起動し、デフォルトで `http://otelop:4318/v1/traces` へOTLP/HTTP exportする。

```bash
BENCH_OTEL_RUN_ID=local-dml ./bench/compare-spanner-dml-unary-shape.sh
BENCH_OTEL_RUN_ID=local-small-select ./bench/compare-small-select-streaming.sh
```

## Runner

単独実行で比較対象を切り替える場合は `BENCH_IMPLEMENTATION=ext-grpc` を指定する。ext-grpc 側は `dev-ext-grpc` と `vendor/autoload.php` を使う。実行後は同じrun idのspanを `otelop-summary.php` で集計する。

```bash
./bench/run.sh throughput-unary --duration=5 --payload-bytes=100
BENCH_IMPLEMENTATION=ext-grpc ./bench/run.sh throughput-unary --duration=5 --payload-bytes=100
```

## OTEL export

ベンチrunnerはPHP runner側の共通RPC境界を 1 RPC = 1 span として記録し、php-grpc-lite / ext-grpc を同じ境界で比較する。UIは `http://localhost:4319`。OTLP/HTTP endpointはcompose内から `http://otelop:4318/v1/traces`、ホストから直接送る場合は `http://localhost:4318/v1/traces` を使う。

```bash
BENCH_OTEL_RUN_ID=local-otel \
./bench/compare-spanner-dml-unary-shape.sh

docker compose run --rm -e BENCH_OTEL_RUN_ID=local-otel dev php \
  tools/benchmark/otelop-summary.php \
  --run-id=local-otel \
  --suite=spanner-dml-unary-shape
```

## 代表ケース

| script / suite | 用途 |
|---|---|
| `compare-spanner-dml-unary-shape.sh` | Spanner DML flow の BeginTransaction / ExecuteSql DML / Commit に近い small unary shape |
| `compare-small-select-streaming.sh` | Spanner `ExecuteStreamingSql` が 1 `PartialResultSet` で返る small SELECT 近似 |
| `throughput-unary` | 単一 PHP process / concurrency=1 の sustained unary throughput と tail latency |
| `rtt-unary` | direct と downstream latency 1 / 3 / 5 ms の unary RTT |
| `throughput-streaming` | server streaming message/sec と stream latency |
| `large-streaming` | large / many-message server streaming のtail latencyとmemory |
| `payload-unary` | unary payload size 別の calls/sec と tail latency |
| `payload-streaming` | streaming payload size 別の messages/sec と stream latency |
| `metadata-header` | request / initial / trailing metadata 数別の unary latency |

`*-diagnostic` suite は実装内部の切り分け用で、通常比較の一次指標にはしない。traceやserver statsを有効にしたrunのlatencyは参考値として扱う。

## ベンチ文書を書く基準

- 対向サーバ、実行コマンド、代表値、揺れ幅を残す。
- ext-grpc は目標値ではなく比較線として扱う。
- Spanner emulator は実機互換検証には使うが、性能比較では Go test-server を優先する。
- cold と warm を混ぜない。request 内で Channel を再利用できる workload は warm、request ごとに 1 RPC の workload は cold を参照する。

## 記録済みの比較

- [Benchmark preliminary comparison 2026-04-28](./benchmark-preliminary-comparison-2026-04-28.md)
- [Benchmark decision comparison 2026-04-29](./benchmark-decision-comparison-2026-04-29.md)
