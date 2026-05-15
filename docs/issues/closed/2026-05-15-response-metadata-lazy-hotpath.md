---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# response metadata lazy化の余地を検証する

## 目的

response metadataを受信時にlinked listへ保存し、完了時にPHP arrayへコピーする固定費について、metadataを読まない通常経路で省略または遅延できるか確認する。

## 背景

`grpc_protocol_add_response_metadata_entry()` は非pseudo headerをすべて保存し、`grpc_protocol_copy_metadata_map()` がPHP arrayへコピーする。status生成にはtrailing metadataが必要だが、initial metadataは多くの実ワークロードで読まれない可能性がある。

## スコープ

- API互換を壊さずに省略可能なcopyがあるか確認する。
- 可能な範囲でmetadata copyを必要時に限定する。

## 非スコープ

- `Grpc\Call::startBatch()` の公開surface変更。
- metadataを返せなくする互換性破壊。

## 計画

1. unary / server streamingでmetadataがどのタイミングで要求されるか確認する。
2. 安全に省略できるcopyだけ実装する。
3. metadata互換テストとCPU microで検証する。

## 進捗

- unary / server streamingのmetadata要求タイミングを確認した。
- 現行APIではstatus生成時にmetadata arrayを返すため、C側で受信metadataを捨てる、またはPHP array生成を完全遅延する変更は公開surfaceへ影響する。
- binary metadataのdecodeは `grpc_protocol_copy_metadata_map()` 側で行われており、受信時点で不要なdecodeをしているわけではない。

## 検証

- `010-unary.phpt` はcontent-type / grpc-statusを含むmetadata返却を検証する。
- `023-metadata-and-call-credentials.phpt` はduplicate metadata / binary metadata互換を検証する。
- `ErrorSemanticsTest` 系はstatusとmetadataの組を前提にする。

## 判断

- API互換を保ったまま「metadataを読まない通常経路だけ完全省略」する余地は小さい。
- 省略可能なのはbinary decodeなどの遅延処理で、これは現行構造ですでに満たしている。
- lazy化を進めるなら、status/result objectの内部表現を変える設計変更になるため、このCPU hot pathの小修正としては採用しない。

## 完了条件

- 省略可能なmetadata copyがあれば削減される。
- 省略できない場合は理由と次善策を記録する。

## 完了

- 安全に省略できる追加copyはないと判断したためClosed。
- 修正コミット: this commit
