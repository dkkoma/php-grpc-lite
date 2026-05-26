# HTTP/2 MAX_FRAME_SIZE / MAX_HEADER_LIST_SIZE INI対応

- Status: Open
- Owner: Codex
- Created: 2026-05-26
- Related:
  - GitHub issue #5

## 目的

grpc-liteが初期HTTP/2 `SETTINGS` で `SETTINGS_MAX_FRAME_SIZE` と `SETTINGS_MAX_HEADER_LIST_SIZE` を明示し、INIで検証可能にする。

## 背景

現行grpc-liteは初期SETTINGSとして `ENABLE_PUSH=0` と `INITIAL_WINDOW_SIZE` だけを送る。`MAX_FRAME_SIZE` と `MAX_HEADER_LIST_SIZE` は未送信であり、HTTP/2 peerには明示的な上限や希望値を伝えていない。

`MAX_HEADER_LIST_SIZE` は未送信の場合、HTTP/2 wire上は明示上限なしとして振る舞う。ただしgrpc-lite内部ではresponse metadata上限を持つため、peerへ伝えるSETTINGSと内部上限の関係が不明瞭だった。

## スコープ

- `grpc_lite.http2_max_frame_size` INIを追加する。
- `grpc_lite.http2_max_header_list_size` INIを追加する。
- どちらもdefault値を持ち、`0` を「未送信」の特別値にしない。
- 初期HTTP/2 SETTINGSで常に送る。
- PHPT / C unit / docsを更新する。

## 非スコープ

- ext-grpcのwire profileに寄せること。
- active PING / BDP estimator / SETTINGS動的更新。
- Spanner専用チューニング。

## 設計

- `grpc_lite.http2_max_frame_size`
  - default: `16384`
  - HTTP/2 defaultかつ仕様上の最小値。
  - 有効範囲は `16384..16777215` にclampする。
- `grpc_lite.http2_max_header_list_size`
  - default: `65536`
  - 現行response metadata default上限と合わせる。
  - `0` は「0を送る」設定値として扱い、未送信の特別値にしない。
  - 負値は `0`、`UINT32_MAX` 超過は `UINT32_MAX` にclampする。

## 完了条件

- 初期SETTINGSに `MAX_FRAME_SIZE` と `MAX_HEADER_LIST_SIZE` が含まれる。
- INI defaultと変更可能性がPHPTで確認される。
- helper境界値がC unitで確認される。
- HTTP/2/gRPCドメインモデル観点で問題がないことをレビューする。

## 検証

- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS, 16/16
- HTTP/2/gRPCドメインモデルレビュー: `docs/reviews/issues/2026-05-26-http2-settings-ini-domain-review.md`, Blocker/High/Medium/Low none

## 完了判断

2026-05-26に完了。`MAX_FRAME_SIZE` と `MAX_HEADER_LIST_SIZE` は初期HTTP/2 SETTINGSとして常に送信され、INIで検証時に変更可能になった。defaultはHTTP/2 defaultのframe size 16KiBと、grpc-lite response metadata defaultに合わせたheader list size 64KiBとした。
