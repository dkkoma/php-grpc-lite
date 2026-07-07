# deadline 超過時に RST_STREAM(CANCEL) を送って接続を温存する

- Status: Open
- Created: 2026-07-08
- Branch: (未着手)
- Owner: Claude

## Background

gRPC のキャンセル（deadline 超過を含む）はストリーム単位の `RST_STREAM` で行い、接続は生かしたまま他のコール／後続のコールで再利用するのが仕様の想定。接続全体を落とすのは接続レベル障害のときのみ。

> RST_STREAM error codes → CANCEL(8): Mapped to call cancellation when sent by a client.
> — [PROTOCOL-HTTP2.md § Errors](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#errors)

現状の実装は unary の受信ループ（`src/unary_call.c` の `grpc_lite_unary_call_perform_core_on_connection`、`nread <= 0` 分岐）で、socket timeout を検出すると `call.timed_out = true` にした後 `mark_connection_dead(connection, errno)` で**接続ごと破棄**する。RST_STREAM は送らない。

persistent connection が前提の FrankenPHP worker 用途では、1 回の DEADLINE_EXCEEDED のたびに TCP + TLS ハンドシェイクからやり直しになり、レイテンシ面の実害がある。また RST_STREAM を送らないため、サーバー側は クライアントが消えるまで処理を継続し得る。

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: deadline 超過はコールのキャンセルとして扱われ、chttp2 トランスポートが該当ストリームに `RST_STREAM(CANCEL)` を送出する。接続（channel/transport）は維持され、後続コールで再利用される。
  - 実装: [chttp2_transport.cc `grpc_chttp2_cancel_stream`](https://github.com/grpc/grpc/blob/master/src/core/ext/transport/chttp2/transport/chttp2_transport.cc)
- **Go 公式 (grpc-go)**: context の期限切れ／キャンセルで `ClientStream.Close(err)` が呼ばれ、`err != nil` のとき `rstCode = http2.ErrCodeCancel` で RST_STREAM を送る。トランスポート（接続）はそのまま。
  - 実装: [internal/transport/client_stream.go `ClientStream.Close`](https://github.com/grpc/grpc-go/blob/master/internal/transport/client_stream.go)（`rstCode = http2.ErrCodeCancel`）

なお本実装の streaming 側にはユーザー起点キャンセル用の `cancel_active_server_streaming_call_state`（`src/transport.c`）が既に RST_STREAM(CANCEL) を実装済みで、deadline 経路だけが接続破棄になっている。

## Goals

- unary / server streaming の deadline 超過時に、接続を殺す代わりに `nghttp2_submit_rst_stream(NGHTTP2_CANCEL)` を送出して当該ストリームのみ閉じる。
- RST 送出後、接続を persistent cache に残して次コールで再利用できるようにする。

## Non-Goals

- deadline 検出精度そのものの変更（poll ベースの現行方式は維持）。
- 送信途中（DATA が書きかけ）で write がブロックしているケースの救済。write ブロック中のタイムアウトは HTTP/2 フレーム境界を保証できないため、従来どおり接続破棄でよい。

## Plan

- unary 受信ループの socket timeout 分岐で、`mark_connection_dead` の代わりに RST_STREAM(CANCEL) を submit → `send_pending_h2_frames` → 短時間（数十 ms 上限）の drain で stream close を待つ。
- drain 中に close が確認できなければ従来どおり接続破棄にフォールバック（読み残しバイトを持つ接続を再利用しないため）。
- streaming の read timeout 経路にも同じ処理を適用。
- PHPT: 遅延応答サーバーに対して timeout → 同じ persistent connection で後続コールが成功（`persistent_reused = true`）することを確認。

## Progress

## Verification

## Decision Log

## Close Criteria

- deadline 超過後に同一 persistent connection が再利用される PHPT が通る。
- timeout 時にワイヤ上へ RST_STREAM(CANCEL) が出ることをトレース（`GRPC_LITE_TRACE_FILE`）で確認。
