# ステータス分類の公式実装との差分是正（compressed flag / HTTP_1_1_REQUIRED / trailers 欠落）

- Status: Open
- Created: 2026-07-08
- Branch: codex/issue-status-taxonomy-official-alignment
- Owner: Claude

## Background

エラー分類（status taxonomy）で、公式実装と食い違う箇所が 3 点ある。いずれも挙動としては「エラーになる」点は同じで、返すコードだけが異なる。ext-grpc からの置き換えでアプリ側のエラーハンドリング（コード別リトライ等）に影響し得る。

### 1. advertise していない圧縮レスポンス → UNIMPLEMENTED（公式は INTERNAL）

`grpc_lite_status_code_from_call`（`src/status_core.c`）は `compressed_response_seen || unsupported_response_encoding` を `UNIMPLEMENTED` にしている。仕様はクライアント側のこのケースを INTERNAL と定める（UNIMPLEMENTED は「サーバーが」クライアントの圧縮を処理できないときにサーバーが返すコード）。

> If a client message is compressed by an algorithm that is not supported by a server, the message WILL result in an UNIMPLEMENTED error status on the server. ... In cases where the client is unable to process [server messages], an INTERNAL error status will occur on the client side.
> — [compression.md § Test cases](https://github.com/grpc/grpc/blob/master/doc/compression.md)（"a message compressed by a server ... using an algorithm not advertised in the client's grpc-accept-encoding MUST fail with INTERNAL" も同文書）

- **Go 公式**: [rpc_util.go `checkRecvPayload`](https://github.com/grpc/grpc-go/blob/master/rpc_util.go) — compressed flag が identity/空エンコーディングと矛盾 → `codes.Internal`。decompressor 未登録は `isServer` で分岐し、**サーバー側は Unimplemented、クライアント側は Internal**。
- **PHP 公式 (ext-grpc = C-core)**: [compression_filter.cc `DecompressMessage`](https://github.com/grpc/grpc/blob/master/src/core/ext/filters/http/message_compress/compression_filter.cc) — 展開失敗は `absl::InternalError`。

### 2. RST_STREAM HTTP_1_1_REQUIRED(0xd) → UNKNOWN（公式は INTERNAL）

`grpc_lite_status_code_from_call` の RST_STREAM コード switch は仕様表（[PROTOCOL-HTTP2.md § Errors](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#errors)）に載るコードを網羅するが、表にない `HTTP_1_1_REQUIRED` が default の `UNKNOWN` に落ちる。

- **Go 公式**: [internal/transport/http_util.go `http2ErrConvTab`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http_util.go) — `http2.ErrCodeHTTP11Required: codes.Internal`。
- **C-core**: [status_conversion.cc `grpc_http2_error_to_grpc_status`](https://github.com/grpc/grpc/blob/master/src/core/lib/transport/status_conversion.cc) — 未知コードのフォールバックも INTERNAL 系（UNKNOWN ではない）。

### 3. :status 200 + trailers なしで END_STREAM → UNKNOWN（grpc-go は INTERNAL）

grpc-status を一度も受け取らずストリームが正常クローズした場合、現状は `grpc_lite_status_code_from_call` の最終 fallback で `UNKNOWN` になる。

- **Go 公式**: [internal/transport/http2_client.go `operateHeaders`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go) — "The server has closed the stream without sending trailers" として `status.New(codes.Internal, "server closed the stream without sending trailers")`。
- 仕様の [PROTOCOL-HTTP2.md](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#responses) は Trailers を必須要素として定義しており（`Response → (Response-Headers *Length-Prefixed-Message Trailers) / Trailers-Only`）、欠落はプロトコル違反＝INTERNAL が自然。

## Goals

- 上記 3 ケースのステータスコードを公式実装（INTERNAL）に揃える。details 文言は現行のものを維持しつつコードのみ変更。

## Non-Goals

- その他のマッピング変更。HTTP ステータス表・RST_STREAM 表のその他の行は仕様・公式実装と一致済み。

## Plan

- `src/status_core.c`: `compressed_response_seen` / `unsupported_response_encoding` を `GRPC_STATUS_INTERNAL` へ。RST switch に `0xd (HTTP_1_1_REQUIRED)` → INTERNAL を追加（または default を INTERNAL に変更し、未知コードも公式に合わせる）。
- trailers 欠落: 「stream_closed かつ grpc_status 未受信かつ http_status==200 かつ他のエラーなし」の判定を追加して INTERNAL を返す。
- C unit テスト（`tests/unit/test_status_core.c`）に 3 ケースを追加。

## Progress

- 2026-07-10: 3ケースすべて実装。
  - `src/status_core.c`: `compressed_response_seen || unsupported_response_encoding` → INTERNAL、RST switch に `NGHTTP2_HTTP_1_1_REQUIRED` → INTERNAL を追加、最終 fallback 直前に「`stream_closed` かつ `stream_error_code == NGHTTP2_NO_ERROR` かつ `http_status == 200`」→ INTERNAL の trailers 欠落判定を追加。
  - `src/transport.c` `grpc_lite_status_details_from_call`: 圧縮系 details（"compressed gRPC messages are not supported" / "unsupported grpc-encoding: ..."）を UNIMPLEMENTED case から INTERNAL case へ移動（文言は維持）。trailers 欠落には "server closed the stream without sending trailers" を追加。
  - テスト更新: `tests/unit/test_status_core.c`（圧縮2件の期待値変更 + HTTP_1_1_REQUIRED + trailers 欠落3アサーション追加）、`tests/Integration/CompressionTest.php` / `tests/phpt/022-error-and-http-validation.phpt` の UNIMPLEMENTED 期待を INTERNAL へ。

## Verification

- `tools/test/check-c-unit.sh`: protocol_core / status_core / transport_core すべてpass（2026-07-10）。
- `tools/test/check-phpt.sh`: 17/17 pass。
- PHPUnit 統合スイート: 31 tests / 116 assertions OK。

## Decision Log

- UNIMPLEMENTED → INTERNAL の変更はアプリから見えるコード変更なのでリリースノート記載が必要。
- trailers 欠落判定は `stream_error_code == NGHTTP2_NO_ERROR`（clean END_STREAM）に限定した。RST 由来の close は `stream_reset_seen` 側の taxonomy が先に効き、ローカルエラーによる close（reset frame なしで error_code 非0）は従来どおり UNKNOWN のまま（issue のスコープ「trailers なしで END_STREAM」に厳密に対応）。
- trailers 欠落の details は従来空文字列だったが、INTERNAL の details fallback（"malformed gRPC response frame"）に落ちると誤解を招くため、grpc-go と同文言 "server closed the stream without sending trailers" を新設した（既存文言の変更ではなく追加）。

## Close Criteria

- 3 ケースの C unit テストが INTERNAL を検証して通る。
- 既存 PHPT / PHPUnit スイートに回帰がない。
