---
Status: Open
Owner: Codex
Created: 2026-05-14
---

# README Performance snapshot 更新

## 目的

READMEのPerformance snapshotを現在の主要ベンチ構成に合わせ、古いSpanner unary shape / small-select表記を消す。

## 背景

`spanner-shape` をBegin/Commit unary、SELECT/DML server streaming構成で再作成したため、READMEに残っている旧Spanner shape結果は誤解を招く。`spanner-real-client` は実経路smoke/regressionとして位置づけを分ける必要がある。

## スコープ

- READMEのPerformance snapshotを2026-05-14時点の主要構成に更新する。
- Spanner代表性能は `spanner-shape` の結果を載せる。
- 実経路は性能主指標ではなくsmoke/regressionとして注記する。

## 非スコープ

- 非Spanner主要ベンチの再計測。
- README以外の広範なドキュメント再整理。

## 検証

- README差分確認。

## 完了条件

- READMEから旧Spanner unary DML / small-select shapeの誤解が消えている。
- 最新docsへの参照がある。

## 進捗

- README Performance snapshotを2026-05-14の `spanner-shape` 結果中心に更新した。
- `spanner-real-client` は実経路smoke/regressionとして位置づけを明記した。

## 検証

- README差分確認。

## Fix summary

READMEのPerformance snapshotから旧Spanner shape表記を外し、最新の `spanner-shape` / `spanner-real-client` の役割に合わせた。

## Fix commit

- Bench: README Performance snapshotを最新Spanner shapeに更新
