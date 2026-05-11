# prune benchmark suites and options

Status: Closed

## 目的

現行ベンチマーク基盤から旧診断用suiteと未使用optionを削除し、通常ベンチとして実行すべきsuiteだけを `bench/run.sh` / `bench/compare.sh` から扱えるようにする。

## 背景

OTEL-only移行とbenchmark構造整理後も、libcurl時代やC内部diagnostic時代のsuite/optionが `bench/run.sh` と PHP runner に残っている。現在の性能比較ではOTEL span durationを一次ソースにし、Spanner DML / small SELECT shape は通常の代表ベンチとして扱うべきである。

## スコープ

- `bench/run.sh` の公開suiteを通常ベンチに絞る。
- Spanner DML unary shapeとsmall SELECT streaming shapeを通常suiteとして追加する。
- 不要なdiagnostic suiteと対応optionを削除する。
- 未使用PHP runnerを削除または通常用途へ改名する。
- docsと検証導線を更新する。

## 非スコープ

- 新しいベンチケースの追加。
- OTEL export / summary形式の変更。
- C拡張bench-only entrypointの整理。

## 計画

- 通常suiteと削除suiteを分類する。
- `bench/run.sh` / `bench/compare.sh` の導線を整理する。
- PHP runnerから未使用optionを削除する。
- docsを更新する。
- lintと短いsmokeで検証する。

## 進捗

- `bench/run.sh` から旧diagnostic suiteを削除した。
- `spanner-dml-unary-shape` と `small-select-streaming` を通常suiteへ追加した。
- `tools/benchmark/streaming-diagnostic.php` を `tools/benchmark/small-select-streaming.php` へ改名し、small SELECT 1 message shapeをデフォルトにした。
- `tools/benchmark/request-unary.php` を削除した。
- unary / payload / metadata runnerから旧diagnostic optionを削除した。
- 現行docsでSpanner shapeを通常suiteとして案内するよう更新した。

## 検証

- `docker compose run --rm dev sh -lc 'for f in tools/benchmark/*.php; do php -l "$f" || exit 1; done; bash -n bench/run.sh bench/compare.sh bench/compare-spanner-dml-unary-shape.sh bench/compare-small-select-streaming.sh'`
- `BENCH_OTEL_RUN_ID=prune-bench-spanner-smoke-20260510213804 ./bench/run.sh spanner-dml-unary-shape --duration=0.01 --warmup-calls=1 --max-calls=2`
- `BENCH_OTEL_RUN_ID=prune-bench-small-select-smoke-20260510213814 ./bench/run.sh small-select-streaming --streams=1 --warmup-streams=1`
- `BENCH_OTEL_RUN_ID=prune-bench-compare-spanner-smoke-20260510213829 ./bench/compare.sh spanner-dml-unary-shape --duration=0.01 --warmup-calls=1 --max-calls=1`

## 判断ログ

- Spanner DML / small SELECT shapeは通常ベンチに含める。
- `*-diagnostic` suiteとlibcurl由来optionは現行の通常ベンチ導線から削除する。

## 完了条件

- `bench/run.sh` に不要なdiagnostic suiteが残らない。
- 現行runnerに未実装optionが残らない。
- Spanner shapeを `bench/run.sh` / `bench/compare.sh` で実行できる。
- lintと代表smokeが通る。

## 修正コミット

このコミット。
