# grpc-accept-encoding の送出と圧縮レスポンス対応

- Status: Open
- Created: 2026-07-08
- Branch: (未着手)
- Owner: Claude

## Background

現状の実装はリクエストに `grpc-accept-encoding` を一切送らず（`src/unary_call.c` / `src/server_streaming_call.c` の固定ヘッダ列に含まれない）、Compressed-Flag=1 のメッセージを受信すると `compressed_response_seen` を立てて一律拒否する（`src/transport.c` の `grpc_protocol_process_response_data_direct` / `grpc_protocol_validate_response_message_lengths`）。

仕様上 `Message-Accept-Encoding` はオプショナルであり（[PROTOCOL-HTTP2.md § Requests](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#requests)）、advertise しないクライアントに準拠サーバーが圧縮メッセージを送ることはないため、現状でも interop 上は安全側。ただし:

- 圧縮を advertise できないため、大きなレスポンス（Spanner の結果セット等）で帯域面の恩恵を受けられない。
- 公式クライアントからの置き換え時に、サーバーから見える能力が変わる（下記差異）。

圧縮の仕様: [compression.md](https://github.com/grpc/grpc/blob/master/doc/compression.md)

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: compression filter がクライアント initial metadata に常に `grpc-accept-encoding`（デフォルトで identity,deflate,gzip）を付与し、受信側で自動 decompress する。`grpc.default_compression_algorithm` 等の channel args で送信圧縮も可能。
  - 実装: [compression_filter.cc `ChannelCompression::HandleOutgoingMetadata`](https://github.com/grpc/grpc/blob/master/src/core/ext/filters/http/message_compress/compression_filter.cc)（`outgoing_metadata.Set(GrpcAcceptEncodingMetadata(), enabled_compression_algorithms())` — 無条件に送出）、同ファイル `DecompressMessage`
- **Go 公式 (grpc-go)**: 登録済み compressor がある場合のみ `grpc-accept-encoding` を送る（gzip は `encoding/gzip` を import した時のみ登録）。未登録なら送らない＝現在の php-grpc-lite に近い挙動。
  - 実装: [internal/transport/http2_client.go `createHeaderFields`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go)（`hpack.HeaderField{Name: "grpc-accept-encoding", Value: registeredCompressors}`）

つまり ext-grpc からの drop-in replacement を厳密に考えると、gzip 受信対応 + advertise が既定で有効という点が差異になる。

## Goals

- 段階 1: `grpc-accept-encoding: identity` を明示送出する（実装コスト最小、能力を正しく表明）。
- 段階 2: zlib(gzip) による受信 decompress を実装し、`grpc-accept-encoding: identity,gzip` を advertise する。Compressed-Flag=1 かつ `grpc-encoding: gzip` のメッセージを展開して返す。

## Non-Goals

- 送信側（リクエスト）圧縮。ext-grpc でも既定は無圧縮であり、クライアント→サーバー方向のペイロードは小さいのが主用途。
- deflate / snappy 等 gzip 以外のアルゴリズム。

## Plan

- 固定リクエストヘッダに `grpc-accept-encoding` を追加（unary / streaming 共通化されている `h2_request_headers` 組み立てに 1 行）。
- 段階 2 では decompress 後サイズにも `max_receive_message_bytes` を適用する（C-core の `DecompressMessage` 同様、展開後サイズで RESOURCE_EXHAUSTED 判定）。
- 展開失敗は INTERNAL（[compression.md](https://github.com/grpc/grpc/blob/master/doc/compression.md) のクライアント側エラー分類に従う）。

## Progress

## Verification

## Decision Log

## Close Criteria

- リクエストヘッダに `grpc-accept-encoding` が載ることをトレースで確認する PHPT。
- （段階 2）gzip 圧縮レスポンスを返す Go テストサーバーに対する unary / streaming の PHPT が通り、展開後サイズ超過が RESOURCE_EXHAUSTED になる。
