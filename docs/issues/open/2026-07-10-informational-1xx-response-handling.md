# 1xx (informational) 応答の HEADERS を final response として誤処理しない

- Status: Open
- Created: 2026-07-10
- Branch: codex/issue-status-taxonomy-official-alignment（PR #28 に同梱で実装済み）
- Owner: Claude

## Background

[2026-07-08-status-taxonomy-official-alignment](2026-07-08-status-taxonomy-official-alignment.md)（PR #28）の敵対的再レビュー [Medium] 指摘（`NGHTTP2_HCAT_HEADERS` ≠ terminal trailers）への対応中に修正した、既存の制限。terminal frame 判別（END_STREAM ゲート）自体は taxonomy issue のスコープだが、1xx 対応は独立した新規対応なので記録として issue を分割する。

nghttp2 の category 契約では、最初の response HEADERS だけが `NGHTTP2_HCAT_RESPONSE` で、1xx (informational) の場合は後続の non-final block と final response HEADERS がすべて `NGHTTP2_HCAT_HEADERS` で届く。従来の実装は:

1. `HCAT_RESPONSE`（= 1xx block）に対して content-type validation を実行するため、content-type を持たない 1xx で `invalid_content_type` が誤発火していた。
2. 1xx 後の final response HEADERS を trailing HEADERS として扱っていた。

gRPC サーバーが 1xx を送ることは実運用上ほぼないため実害は小さいが、terminal frame 判別の正しさをテストで固定するには 1xx 経由の経路が必要だった。

## Goals

- `HCAT_RESPONSE` の `:status` が 1xx の場合は `expect_final_response` を立てて response validation を保留し、後続の `HCAT_HEADERS` を final response headers として validate する。
- 1xx 経由でも正常応答（grpc-status 0）が成功し、trailers 欠落は DATA END_STREAM 判定（INTERNAL）が正しく機能する。

## Non-Goals

- 100-continue 等、1xx への能動的な応答動作。受動的に無視して final response を待つのみ。

## Progress

- 2026-07-10: PR #28 内で実装済み。
  - `src/grpc_exchange_state.h`: `expect_final_response` を追加。
  - `src/transport.c` `on_frame_recv_callback()`: 1xx の `HCAT_RESPONSE` で validation を保留し、`expect_final_response` 中の `HCAT_HEADERS` を final response として処理。trailing HEADERS の記録は `NGHTTP2_FLAG_END_STREAM` 付きに限定。
  - fixture: `x-bench-early-hints=1`（103 Early Hints を先行送出、他 control と併用可）。
  - PHPT 022: 1xx + no-trailers → INTERNAL / 1xx + status0 → OK を unary / streaming で固定。
  - `docs/design/grpc-call-exchange-state.md` の responsibility map に `expect_final_response` / `trailing_headers_seen` を追加。

## Verification

- PR #28 の検証に含む（C unit / PHPT 17/17 / PHPUnit / static analysis pass、2026-07-10）。

## Decision Log

- PR #28 と分離せず同梱のままとし、記録のみ issue 分割する（2026-07-10、PR スコープ膨張の指摘を受けて）。
- 連続 1xx（103 → 103 → 200 等）は `expect_final_response` を維持して final まで待つ。1xx block 内の headers は response metadata に記録され得るが、実運用で 1xx を送る gRPC サーバーは想定しないため許容する。

## Close Criteria

- PR #28 のマージ。
