# Benchmark Guide

このディレクトリはベンチ結果の記録、`bench/` は実行対象、`bench/run.sh` は再現可能な実行入口を置く。

## 通常の比較線

通常の継続比較は **php-grpc-lite vs 公式 ext-grpc** に固定する。Rust 製 drop-in 代替候補 `grpc-php-rs` は one-shot / 明示依頼時だけ `bench/compare-rs.sh` で使う。

```bash
./bench/run.sh compare
```

互換のため、従来の入口も同じ比較線を実行する。

```bash
./bench/compare.sh
```

どちらも `var/bench-results/` にログを保存し、PHPBench aggregate については同名の `.json` / `.tsv` も生成する。保存名は `BENCH_TAG` と `BENCH_OUTPUT_DIR` で固定できる。

```bash
BENCH_TAG=20260427-local ./bench/run.sh compare
BENCH_OUTPUT_DIR=/tmp/php-grpc-lite-bench ./bench/run.sh cold
```

## スイート

| command | 対象 | 用途 |
|---|---|---|
| `./bench/run.sh lite` | php-grpc-lite full PHPBench | 実装側だけの全体確認 |
| `./bench/run.sh ext` | 公式 ext-grpc full PHPBench | 比較対象だけの確認 |
| `./bench/run.sh compare` | php-grpc-lite + 公式 ext-grpc | 通常比較 |
| `./bench/run.sh cold` | `ColdUnaryBench` 両環境 | PHP-FPM request boundary 近似 |
| `./bench/run.sh warm` | `UnaryLatencyBench` 両環境 | request 内 Channel reuse が効く軽量 unary |
| `./bench/run.sh stream` | `ServerStreamingBench` 両環境 | server streaming の per-message / per-byte / pacing |
| `./bench/run.sh stream-smoke` | `ServerStreamingCount1000Bench` 両環境 | server streaming count=1000 の回帰 smoke |
| `./bench/run.sh hot-path` | `tools/bench-hot-path.php` | ネットワークなしの CPU 分解 |

## 生成物

`./bench/run.sh cold` を `BENCH_TAG=local` で実行すると、以下のようなファイルができる。

```text
var/bench-results/cold-local-php-grpc-lite.log
var/bench-results/cold-local-php-grpc-lite.json
var/bench-results/cold-local-php-grpc-lite.tsv
var/bench-results/cold-local-ext-grpc.log
var/bench-results/cold-local-ext-grpc.json
var/bench-results/cold-local-ext-grpc.tsv
```

手動で既存ログを変換する場合は以下を使う。

```bash
docker compose run --rm dev php tools/parse-phpbench-aggregate.php \
  --format=json \
  --output=var/bench-results/result.json \
  var/bench-results/result.log
```

## Regression Baseline

回帰判定用 baseline は `bench/baselines/regression.json` に置く。これは historical baseline ではなく、意図した改善や環境変更後に明示更新してコミットする基準値。

`BENCH_BASELINE` を指定すると、php-grpc-lite 側の抽出済み JSON を baseline と比較する。

```bash
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh cold
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh warm
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh stream-smoke
```

手動比較もできる。

```bash
docker compose run --rm dev php tools/compare-benchmark-baseline.php \
  --baseline=bench/baselines/regression.json \
  --current=var/bench-results/cold-local-php-grpc-lite.json \
  --suite=cold \
  --implementation=php-grpc-lite
```

baseline は php-grpc-lite 自身の回帰検知用で、公式 ext-grpc との差を fail 判定するためには使わない。ext-grpc は継続比較線として観測する。

## ベンチ文書を書く基準

- 対向サーバ、実行コマンド、代表値、揺れ幅を残す。
- ext-grpc は目標値ではなく比較線として扱う。
- Spanner emulator は実機互換検証には使うが、性能比較では Go test-server を優先する。
- cold と warm を混ぜない。request 内で Channel を再利用できる workload は warm、request ごとに 1 RPC の workload は cold を参照する。

## Phase 1 の残り

- CI で回すスモークベンチのしきい値を決める。
- regression baseline に載せた smoke 対象を CI/manual でどう回すか決める。
- メモリ指標は PHPBench aggregate の `mem_peak` を記録しているが、回帰判定にはまだ使っていない。
