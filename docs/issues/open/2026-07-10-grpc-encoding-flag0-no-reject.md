# grpc-encoding 宣言のみでレスポンスを拒否しない（flag=0 message の成功）

- Status: Open
- Created: 2026-07-10
- Branch: codex/issue-status-taxonomy-official-alignment（PR #28 に同梱で実装済み）
- Owner: Claude

## Background

[2026-07-08-status-taxonomy-official-alignment](2026-07-08-status-taxonomy-official-alignment.md)（PR #28）の敵対的レビュー [Medium] 指摘から派生した、独立した挙動修正。本来は別 PR に値するスコープだが、PR #28 のレビューサイクル内で実装したため、記録として issue を分割する。

従来の実装は `grpc-encoding` header を見た時点（header callback）で `unsupported_response_encoding` を立てて body を discard していた。しかし仕様上 `grpc-encoding` は「圧縮に使われうるアルゴリズムの宣言」であり、実際に圧縮されているかは各 message の Compressed-Flag が決める。grpc-go `checkRecvPayload` も compressor lookup は flag=1 の場合のみで、ext-grpc 1.80.0 実測（レビュー probe）でも gzip 宣言 + flag=0 + `grpc-status: 0` は OK になる。

つまり「エラー → 成功」への挙動変更であり、ステータスコードの張り替え（taxonomy issue 本体）とは性質が異なる。

## Goals

- `grpc-encoding` header は観測のみとし、失敗判定は DATA parser が Compressed-Flag=1 を確認した時点で立てる。
- flag=1 の失敗は encoding 宣言の有無で details を振り分ける（宣言あり → "unsupported grpc-encoding: ..."、なし → "compressed gRPC messages are not supported"、いずれも INTERNAL）。

## Non-Goals

- gzip decompress の実装（[2026-07-08-grpc-accept-encoding-advertise-and-gzip](2026-07-08-grpc-accept-encoding-advertise-and-gzip.md) のスコープ）。

## Progress

- 2026-07-10: PR #28 内で実装済み。
  - `src/transport.c`: header callback の即時拒否を除去、`grpc_protocol_flag_compressed_message()` を追加して flag=1 検出時に振り分け。direct decode loop の停止条件に `unsupported_response_encoding` を追加。
  - fixture: `x-bench-grpc-encoding` + `x-bench-grpc-status` 併用、`compressed-flag` + `x-bench-grpc-encoding` 併用、`headers-only`（trailers-only 応答）。
  - PHPT 022: gzip + flag=0 + status0 → OK / gzip + trailers-only non-OK → wire status / gzip + flag=1 → INTERNAL を unary / streaming で固定。

## Verification

- PR #28 の検証に含む（C unit / PHPT 17/17 / PHPUnit / static analysis pass、2026-07-10）。

## Decision Log

- PR #28 と分離せず同梱のままとし、記録のみ issue 分割する（2026-07-10、PR スコープ膨張の指摘を受けて）。

## Close Criteria

- PR #28 のマージ。
