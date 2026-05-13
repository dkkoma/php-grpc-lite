---
Status: Closed
Owner: Codex
Created: 2026-05-14
---

# Spanner shape benchmark を仕様に合わせて再作成する

## 目的

Spanner実経路とは別に、transport観測向けのSpanner代表shape benchmarkを主要suiteとして再作成する。

## 背景

旧 `spanner-dml-unary-shape` は DML を unary shape として扱っていたが、実際の `google/cloud-spanner` のDMLは `ExecuteStreamingSql` のserver streaming responseをdrainしてstatsを読む。実経路 `spanner-real-client` は上位ライブラリとemulatorのノイズが大きいため、transport観測用には synthetic shape が必要だが、gRPC method shapeは実態に合わせる必要がある。

## スコープ

- `spanner-shape` suiteを追加する。
- BeginTransaction / Commit は unary shape として計測する。
- SELECT / DML insert / update / delete は server streaming shape として計測する。
- `bench/run.sh` と benchmark docsに通常suiteとして登録する。
- `php-grpc-lite` と公式 `ext-grpc` の比較結果を記録する。

## 非スコープ

- Spanner emulator 実経路の置き換え。
- 正確なSpanner protobuf messageの完全再現。
- franken-go backend比較。

## 計画

1. unary/streaming混在の `tools/benchmark/spanner-shape.php` を追加する。
2. `bench/run.sh` に `spanner-shape` を登録する。
3. docsに `spanner-shape` と `spanner-real-client` の役割差を明記する。
4. Dockerでsmokeと比較計測を実行する。

## 進捗

- `tools/benchmark/spanner-shape.php` を追加した。
- `bench/run.sh` に `spanner-shape` を通常suiteとして登録した。
- `AGENTS.md` と `docs/benchmarks/README.md` に `spanner-shape` と `spanner-real-client` の役割差を記録した。
- `docs/benchmarks/spanner-shape-2026-05-14.md` に比較結果を記録した。

## 検証

- `bash -n bench/run.sh bench/compare.sh`
- `docker compose run --rm dev php -l tools/benchmark/spanner-shape.php`
- `BENCH_TAG=spanner-shape-smoke-20260514073711 ./bench/compare.sh spanner-shape --calls=2 --warmup-calls=1`
- `BENCH_TAG=spanner-shape-20260514 ./bench/compare.sh spanner-shape`

## 判断ログ

- BeginTransaction / Commit は unary RPC shape として扱う。
- SELECT / DML は `ExecuteStreamingSql` 相当の server streaming shape として扱う。
- 実経路の互換・回帰確認は `spanner-real-client`、transport寄りの主要性能観測は `spanner-shape` を使う。
- DMLのrequest bytesは旧shapeの値を引き継ぐが、responseは1 message server streamingとして測る。

## 完了条件

- `./bench/compare.sh spanner-shape` で比較できる。
- begin/commit/select/dml insert/update/delete がOTEL summaryに出る。
- 結果docsが残っている。

## Fix summary

Spanner代表shapeを、Begin/Commitはunary、SELECT/DMLはserver streamingとして測る `spanner-shape` suiteとして再作成した。

## Fix commit

- Bench: Spanner shapeをunary/streaming構成で再作成
