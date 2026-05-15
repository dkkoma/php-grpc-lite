---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# startBatch result constructionの空metadata/status固定費を削減する

## 目的

`Call::startBatch()` の返却object構築で、空metadataやstatus object周辺のzval allocation/copyを減らす。

## 背景

mixed transactionでは複数RPCが短時間に実行される。現行実装はAPI互換shapeを満たすため、空metadataでも配列を構築し、status objectにもmetadataを付ける。ext-grpcとのCPU差の一部がC/PHP bridge固定費にある可能性が高い。

## スコープ

- API shapeを変えずに、空metadata配列・status metadataの生成回数を減らせるか調査する。
- 可能ならstatic empty array相当、move semantics、copy回避を導入する。

## 非スコープ

- `Grpc\\Call::startBatch()` の公開戻り値shape変更。
- ext-grpc非互換の省略。

## 検証

- PHPT / PHPUnit。
- real Spanner mixed transaction c16。

## 完了条件

- 互換性を維持したまま固定費削減可否が判断される。
- 採用/見送り理由と計測値が残る。
