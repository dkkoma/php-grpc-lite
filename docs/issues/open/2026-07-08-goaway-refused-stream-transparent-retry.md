# GOAWAY / REFUSED_STREAM で拒否されたコールの透過リトライ

- Status: Open
- Created: 2026-07-08
- Branch: (未着手)
- Owner: Claude

## Background

サーバーが GOAWAY を送ると、`last_stream_id` より大きいストリームは「サーバーが一切処理していない」ことが保証される。仕様はこのケースを UNAVAILABLE 扱いとした上で「retry the call elsewhere」とクライアント側のリトライを明示している。

> Clients should consider any stream initiated after the last successfully accepted stream as UNAVAILABLE and retry the call elsewhere.
> — [PROTOCOL-HTTP2.md § GOAWAY Frame](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#connection-management)

現状の実装は `on_frame_recv_callback`（`src/transport.c` の GOAWAY 分岐）で該当ストリームに `stream_refused_seen` を立てて閉じ、`grpc_lite_status_code_from_call`（`src/status_core.c`）で `UNAVAILABLE` をそのままアプリに返す。リトライは行わない。

Google のフロントエンドは max connection age により定期的に GOAWAY を送るため、persistent connection + Spanner という本プロジェクトの主用途では、負荷時に散発的な UNAVAILABLE がアプリ層へ漏れる。

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: C-core は gRFC A6 の "transparent retry" を実装しており、「RPC がサーバーに一度も到達していない」場合（GOAWAY refused / REFUSED_STREAM を含む）は retry policy の設定に関わらず新しい接続で再送する。
  - 仕様: [gRFC A6 §Transparent Retries](https://github.com/grpc/proposal/blob/master/A6-client-retries.md#transparent-retries) — "RPC never leaves the client" は無制限、"reached the server connection but never seen by server" は 1 回だけ透過リトライ。
  - 実装: [retry_filter.cc](https://github.com/grpc/grpc/blob/master/src/core/client_channel/retry_filter.cc)
- **Go 公式 (grpc-go)**: `handleGoAway` が `LastStreamID` より大きいストリームに `unprocessed` フラグを立て、`csAttempt.shouldRetry` が「初回試行かつ unprocessed」なら透過リトライする。`disableRetry` 設定でも透過リトライ用に送信メッセージをバッファし続ける。
  - [internal/transport/http2_client.go `handleGoAway`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go)（`streamID > id && streamID <= upperLimit` → `stream.unprocessed.Store(true)`）
  - [stream.go `csAttempt.shouldRetry`](https://github.com/grpc/grpc-go/blob/master/stream.go)（`if cs.firstAttempt && unprocessed { return true, nil }`）

つまり公式実装ではこのケースはアプリに UNAVAILABLE として見えず、php-grpc-lite だけがエラーを露出する。

## Goals

- unary / server streaming の開始時、`stream_refused_seen`（GOAWAY refused、および RST_STREAM `REFUSED_STREAM`）で終わったコールを、新しい接続（または draining でない別の persistent connection）で 1 回だけ自動再送する。
- 再送は deadline の残時間内でのみ行う（`grpc-timeout` は残時間で再計算）。

## Non-Goals

- gRFC A6 の retry policy（指数バックオフ、retryableStatusCodes 等）の実装。
- サーバーがすでに処理を開始した可能性のあるコール（データ受信済み等）のリトライ。

## Plan

- `grpc_lite_unary_call_perform_on_connection` / `server_streaming_call_open_resource` の呼び出し側に、`stream_refused_seen && !initial_grpc_status_seen` を条件とする 1 回限りの再試行ループを追加。
- 再試行時は draining 接続をキャッシュから外し、新規接続を確立する（既存の `remove_unusable_persistent_connection` / `get_persistent_connection` を利用）。
- Go テストサーバーに GOAWAY 送出シナリオを追加し PHPT で検証。

## Progress

## Verification

## Decision Log

## Close Criteria

- GOAWAY refused のコールがアプリに UNAVAILABLE を返さず、新接続で成功する PHPT がある。
- 再試行は 1 回限りで、deadline 超過時は DEADLINE_EXCEEDED になることをテストで確認。
