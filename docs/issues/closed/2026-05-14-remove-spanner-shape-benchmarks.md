---
Status: Closed
Owner: Codex
Created: 2026-05-14
---

# Spanner shape benchmark の通常suite削除

## 目的

Spanner実利用経路と乖離した synthetic shape benchmark を通常ベンチマークから外し、`google/cloud-spanner` の高レベルAPI経由を主要ケースにする。

## 背景

`spanner-dml-unary-shape` と `small-select-streaming` はGo test-serverに対する合成RPCで、payload shapeやsmall streamingのtransport観測には使えた。一方、実際の `google/cloud-spanner` はGAX transport、`ServerStream` wrapper、`Result::rows()` drain、transaction commitを含むため、Spanner代表ケースとしては `spanner-real-client` のほうが責務境界に合っている。

## スコープ

- `bench/run.sh` から `spanner-dml-unary-shape` と `small-select-streaming` を削除する。
- 専用比較runnerと対応するbenchmark scriptを削除する。
- active benchmark docsを `spanner-real-client` 中心に更新する。

## 非スコープ

- 過去のresearch / issue / benchmark記録の削除。
- 汎用transport benchmarkの再設計。

## 計画

1. active runnerからsynthetic Spanner suiteを削除する。
2. 不要なbenchmark scriptと専用compare scriptを削除する。
3. docsの通常実行例とsuite一覧を更新する。
4. shell/PHP syntaxと主要suite smokeを確認する。

## 進捗

- `bench/run.sh` から `spanner-dml-unary-shape` と `small-select-streaming` を削除した。
- `tools/benchmark/unary-shape.php` と `tools/benchmark/small-select-streaming.php` を削除した。
- `bench/compare-spanner-dml-unary-shape.sh` と `bench/compare-small-select-streaming.sh` を削除した。
- `AGENTS.md`、`docs/benchmarks/README.md`、`docs/opentelemetry-instrumentation.md` を `spanner-real-client` 中心に更新した。

## 検証

- `bash -n bench/run.sh bench/compare.sh`
- `docker compose run --rm dev sh -lc 'for f in tools/benchmark/*.php; do php -l "$f" || exit 1; done'`
- `BENCH_TAG=spanner-real-after-prune-20260514072308 ./bench/run.sh spanner-real-client --calls=1 --warmup-calls=0`

## 判断ログ

- 過去の調査記録は履歴として残す。
- small server streamingのtransport観測が必要になった場合は、Spanner名ではなく汎用suiteとして再設計する。

## 完了条件

- `bench/run.sh` のusageにshape suiteが出ない。
- `tools/benchmark/unary-shape.php` と `tools/benchmark/small-select-streaming.php` がactive runnerから消えている。
- `spanner-real-client` が通常suiteとして残っている。

## Fix summary

Spanner代表ケースを `spanner-real-client` に一本化し、実利用経路と乖離したsynthetic shape suiteと専用runnerを削除した。

## Fix commit

- 未コミット
