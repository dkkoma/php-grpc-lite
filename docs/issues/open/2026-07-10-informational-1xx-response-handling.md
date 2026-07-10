# 1xx (informational) 応答の HEADERS を final response として誤処理しない

- Status: Open
- Created: 2026-07-10
- Branch: (未着手。PR #28 に一時同梱後、revert して本 issue へ切り出し)
- Owner: Claude

## Background

[2026-07-08-status-taxonomy-official-alignment](2026-07-08-status-taxonomy-official-alignment.md)（PR #28）の敵対的再レビュー [Medium] 指摘（`NGHTTP2_HCAT_HEADERS` ≠ terminal trailers）への対応中に一度実装したが、第三パスレビュー（protocol-adversary `REVIEW-20260710-004`）で「frame-end 判定だけの不完全な 1xx 成功経路」と指摘され、PR #28 からは revert して本 issue の別 PR スコープとした。

nghttp2 の category 契約では、最初の response HEADERS だけが `NGHTTP2_HCAT_RESPONSE` で、1xx (informational) の場合は後続の non-final block と final response HEADERS がすべて `NGHTTP2_HCAT_HEADERS` で届く。現状の実装は:

1. `HCAT_RESPONSE`（= 1xx block）に対して content-type validation を実行するため、content-type を持たない 1xx で `invalid_content_type` が誤発火し、1xx を挟む応答は失敗する（既知の制限として許容中）。
2. `on_header_callback()` は raw category だけで trailing / initial を決めて即時に call state へ反映するため、frame 完了後にしか分からない「この block は 1xx / final / trailing のどれか」という semantic phase を知らない。

## 却下された初回実装（参考）

PR #28 の commit `375c3dd` で `expect_final_response` フラグによる frame-end 判定を実装したが、以下の理由で revert（レビュー実測 probe 付き）:

- 1xx 後の final response HEADERS のフィールドが trailing metadata として保存される（`x-bench-observe-authority` + 103 併用で `x-bench-authority` が `getMetadata()` から `getTrailingMetadata()` へ移動）。
- 1xx block 内の `content-type` / `grpc-status` / `grpc-message` / `grpc-encoding` が final response の validation / status / compression 分類を汚染しうる（RFC 8297 §2: 103 のフィールドは final response の処理に影響してはならない）。
- frame-end の `on_frame_recv_callback()` では、既に行われた header callback の metadata 追加や semantic field 更新を修復できない。

## Goals

- response header block に call-local な semantic phase（informational / final initial / trailing）を持たせ、`on_begin_headers_callback()` 等で **header callback 時点までに** phase を確定する。
- informational block のフィールドを隔離する（metadata / validation / status state に反映しない）。
- 1xx 後の final response `HCAT_HEADERS` を initial response headers として処理する（metadata は initial 側、validation も initial 相当）。
- unary / server streaming で status / details に加えて **metadata ownership（initial / trailing の帰属）** と 1xx field isolation をテストで固定する。

## Non-Goals

- 100-continue 等、1xx への能動的な応答動作。受動的に無視して final response を待つのみ。

## Plan

- `on_begin_headers_callback()`（または frame header 到達時点）で block phase を決定: 未 final なら `:status` を見る前に informational 候補として開始し、`:status` 確定時に phase を確定する方式を検討（`:status` は block 先頭で届く保証があるため header callback 内での確定も可）。
- fixture: `x-bench-early-hints=1`（103 先行送出、他 control と併用可、PR #28 から revert したものを復活）に加え、1xx block に semantic field（content-type / grpc-status 等）を含める汚染ケースを追加。
- PHPT: 1xx + no-trailers → INTERNAL / 1xx + status0 → OK / metadata ownership（`x-bench-observe-authority` 併用で initial 帰属を assert）/ 1xx field isolation を unary / streaming で固定。

## Progress

- 2026-07-10: PR #28 内で `expect_final_response` による frame-end 判定を一度実装（`375c3dd`）→ 第三パスレビュー `REVIEW-20260710-004` の指摘（上記「却下された初回実装」）を受けて PR #28 から revert。terminal frame 判別（`trailing_headers_seen` の END_STREAM ゲート）のみ PR #28 に残した。

## Verification

## Decision Log

- 2026-07-10: 記録のみの issue 分割（コードは PR #28 同梱のまま）としたが、第三パスレビューで不完全性が実証されたため、コードごと本 issue の別 PR スコープへ変更。

## Close Criteria

- 1xx を挟む応答で metadata ownership / field isolation / status が正しいことを固定する PHPT が unary / server streaming で通る。
- 既存スイート（C unit / PHPT / PHPUnit）に回帰がない。
