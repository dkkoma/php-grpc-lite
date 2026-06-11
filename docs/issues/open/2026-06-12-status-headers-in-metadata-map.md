---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: fix/status-headers-in-metadata-map
Related: docs/issues/closed/2026-05-14-metadata-conversion-hotpath.md
---

# grpc-status / grpc-message を trailing metadata map から除外する(ext-grpc 互換 + alloc 削減)

## 目的

ステータス専用ヘッダ (`grpc-status`, `grpc-message`) が PHP 側の trailing metadata 配列に混入している現状を、公式 ext-grpc (gRPC C core) の挙動に合わせて除外する。あわせて毎 RPC 2〜3 エントリ分の `emalloc` + `zend_string_init` ×2 を削減する。

## 背景

`on_header_callback` (`src/transport.c:1895`) は `grpc-status` / `grpc-message` を call 状態 (`call->grpc_status`, `call->grpc_message`) に取り込んだ後、**さらに** `grpc_protocol_add_response_metadata_entry` (`transport.c:2758`) で metadata リストにも追加している(`:` 始まりの pseudo-header のみ除外)。その結果:

- PHP の `$status->metadata` / trailing metadata に `grpc-status` / `grpc-message` キーが現れる。
- gRPC C core はこれらを Status / Status-Message として消費し、ユーザー向け metadata からは除去する。公式 ext-grpc からの移行で `metadata` を走査するコードに見え方の差が出る。
- 性能面では毎 RPC で metadata_entry の `emalloc` 1 回 + `zend_string_init` 2 回 ×(該当ヘッダ数)が無駄になっている。`max_response_metadata_bytes` の消費量にもカウントされており、ユーザー metadata が同じ上限設定で入りにくくなる微妙な非互換もある。

## spec照合

- gRPC over HTTP/2 spec: `Trailers` は `Status [Status-Message] *Custom-Metadata` と定義され、`grpc-status` / `grpc-message` は Custom-Metadata とは別カテゴリ。クライアントがこれをユーザー metadata として再露出する必要はない。
- **`grpc-status-details-bin` は除外してはならない**: google rich error model (google.rpc.Status) は PHP では `$status->metadata['grpc-status-details-bin']` 経由で読まれる(GAX `ApiException::createFromApiResponse` が参照)。C core も grpc-status-details-bin は trailing metadata として露出する。現行どおり保持すること。

## 修正方法

1. `on_header_callback` の `grpc-status` / `grpc-message` 分岐で、`grpc_protocol_add_response_metadata_entry` への追加をスキップする(分岐末尾の共通呼び出しの前に `return 0;` するか、`bool store_as_metadata` フラグで制御)。
2. `grpc-status-details-bin` は現行どおり metadata に残す。
3. duplicate `grpc-status` 検出 (`call->grpc_status_seen` → `invalid_grpc_status`) 等の検証ロジックは変更しない。
4. trailing metadata に `grpc-status` が含まれないことを PHPT で固定化(ext-grpc 互換テストがあれば併せて比較)。

## 完了条件

- `$status->metadata` に `grpc-status` / `grpc-message` が現れず、`grpc-status-details-bin` は現れる。
- status code / details の解決 (`resolve_grpc_call_status`) に回帰がない。
- 既存テスト全通過(期待値に grpc-status を含むテストがあれば更新)。

## 測定ベンチマーク

- 本 issue は挙動変更が主目的で、性能は副次効果(毎 RPC 数 alloc 削減)のため専用計測は不要。
- 回帰確認: `./bench/run.sh metadata-header`(metadata 経路の悪化なし)、`./bench/run.sh spanner-shape`。

## Progress

## Verification

## Decision Log
