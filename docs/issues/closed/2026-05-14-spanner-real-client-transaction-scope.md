---
Status: Closed
Owner: Codex
Created: 2026-05-14
---

# spanner-real-client の計測scopeを開始済みtransaction内操作に寄せる

## 目的

実用上の単体wall timeとして、small SELECT と DML を開始済み read-write transaction 内の1操作として計測する。

## 背景

現在の `spanner-real-client` は `Database::execute()` の single-use read-only transaction と、`runTransaction()` + `executeUpdate()` + `commit()` を計測している。これは実API経路ではあるが、単体操作のwall timeとしてはtransaction開始/commitの扱いが混ざる。Spanner利用ではSELECT/DMLともtransaction内で実行されるケースが主要なので、代表ベンチは開始済みtransaction内の `Transaction::execute()` / `Transaction::executeUpdate()` を測るほうが比較軸として明確になる。

## スコープ

- `spanner-real-client` のSELECTを `Transaction::execute()` に変更する。
- DMLは事前に開始したtransaction内の `Transaction::executeUpdate()` だけを計測し、rollbackは計測外にする。
- docsの計測条件と結果を更新する。

## 非スコープ

- transaction begin / commit 自体の性能計測。
- 複数statement transaction全体のend-to-end計測。

## 計画

1. `tools/benchmark/spanner-real-client.php` の計測scopeを修正する。
2. smokeと比較計測を実行する。
3. benchmark docsを更新する。
4. issueをcloseしてコミットする。

## 進捗

- `spanner-real-client` のSELECT計測を `Database::execute()` から `Transaction::execute()` に変更した。
- DML計測を `runTransaction() + executeUpdate() + commit()` から、開始済みtransaction内の `Transaction::executeUpdate()` に変更した。
- 計測区間からtransaction開始とrollbackを外した。
- span attributeに `benchmark.transaction_scope=pre_started_read_write` を追加した。
- benchmark結果docsを更新した。

## 検証

- `docker compose run --rm dev php -l tools/benchmark/spanner-real-client.php`
- `BENCH_TAG=spanner-tx-scope-smoke-20260514072951 ./bench/compare.sh spanner-real-client --calls=2 --warmup-calls=1`
- `BENCH_TAG=spanner-real-client-tx-scope-20260514 ./bench/compare.sh spanner-real-client --calls=100 --warmup-calls=5`

## 判断ログ

- transaction開始とrollbackは計測外に置く。
- 計測対象は `ExecuteStreamingSql` の実行とresponse drain / stats取得までにする。
- transaction開始を計測外にするため、各operationの直前に `Database::transaction()` を呼び、開始済みtransactionに対するstatementだけをspan化する。

## 完了条件

- span attributeで開始済みtransaction内操作であることが分かる。
- SELECT / DML insert / update / delete の比較結果が更新される。

## Fix summary

Spanner実経路ベンチの単体wall timeを、開始済みread-write transaction内の `Transaction::execute()` / `Transaction::executeUpdate()` に寄せた。

## Fix commit

- Bench: Spanner実経路を開始済みtransaction内計測に寄せる
