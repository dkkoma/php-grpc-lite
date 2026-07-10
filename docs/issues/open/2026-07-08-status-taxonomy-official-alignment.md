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

- 2026-07-10: PR #28 レビュー指摘対応。
  - docs 更新: `docs/SPEC.md`（重要前提 + 未決事項の圧縮分類）、`docs/design/protocol-classification-boundary.md`（Unsupported compression → INTERNAL、Missing trailers 行を追加）、`docs/verification/compatibility-control-checklist.md`（missing trailers / 圧縮の期待値）を最終挙動に合わせた。
  - 統合カバレッジ: 50054 fixture（`poc/test-server/main.go`）に `x-bench-grpc-response: no-trailers`（message 送信後 grpc-status なしで clean END_STREAM）を追加し、PHPT 022 で unary / server streaming 両方の `STATUS_INTERNAL` + details "server closed the stream without sending trailers" を assert。

- 2026-07-10: 敵対的レビュー（PR #28, `ce5872d` 時点）対応。
  - [Medium] `grpc-encoding` header 観測だけで `unsupported_response_encoding` を立てる過剰拒否を除去。失敗判定は DATA parser が Compressed-Flag=1 を見た時点に移動し、encoding 宣言あり → `unsupported_response_encoding`（details "unsupported grpc-encoding: ..."）、なし → `compressed_response_seen` に振り分け（`grpc_protocol_flag_compressed_message`）。flag=0 message は未対応 encoding 下でも成功する（grpc-go `checkRecvPayload` 準拠）。
  - [Design Decision] missing trailers の分類 policy は **grpc-go exact** を選択。DATA END_STREAM で trailers 欠落 → INTERNAL（`handleData` 準拠）、HEADERS END_STREAM（headers-only / grpc-status なし trailing HEADERS）→ UNKNOWN（`operateHeaders` 準拠）。`trailing_headers_seen` フラグを追加し、`initial_headers_end_stream` と合わせて terminal frame を区別。
  - [Low] `docs/verification/test-fixtures.md` の 50054 control 表と `docs/verification/verification-matrix.md` に missing trailers / 新 fixture を追加。
  - fixture 追加: `headers-only`（`x-bench-grpc-status` 併用で trailers-only）、`custom-trailers-no-status`、`grpc-message-only-trailers`、`compressed-flag` + `x-bench-grpc-encoding` 併用、`x-bench-grpc-encoding` + `x-bench-grpc-status` 併用。PHPT 022 に unary / streaming の matrix（gzip+flag=0+status0 → OK / gzip+trailers-only non-OK → wire status / gzip+flag=1 → INTERNAL / headers-only → UNKNOWN / custom trailers → UNKNOWN / grpc-message only → UNKNOWN + message details）を追加。

- 2026-07-10: 敵対的再レビュー（PR #28, `f5a2f75` 時点）対応。
  - [Medium] `trailing_headers_seen` を `NGHTTP2_FLAG_END_STREAM` 付き HEADERS のみに限定。nghttp2 の category 契約では 1xx 後の final response HEADERS も `HCAT_HEADERS` で届くため、END_STREAM を見ないと non-terminal HEADERS が missing-trailers INTERNAL 判定を誤って抑止する。あわせて 1xx (informational) 応答対応を追加: `HCAT_RESPONSE` が 1xx なら `expect_final_response` を立てて response validation を保留し、後続 `HCAT_HEADERS` を final response headers として validate する（従来は 1xx block に content-type がないため `invalid_content_type` が誤発火していた）。fixture `x-bench-early-hints=1`（103 を先行送出、他 control と併用可）を追加し、PHPT で「1xx + no-trailers → INTERNAL」「1xx + status0 → OK」を unary / streaming で固定。trailing HEADERS + END_STREAM → UNKNOWN は `custom-trailers-no-status` で固定済み。
  - [Medium] `compatibility-control-checklist.md` を最終 policy に整合: missing trailers を DATA END_STREAM（INTERNAL）と HEADERS END_STREAM（UNKNOWN）に書き分け、compression を per-message Compressed-Flag 基準に修正。`SPEC.md` 未決事項の記述も flag=1 限定であることを明記。
  - [Low] `grpc-call-exchange-state.md` の responsibility map に `trailing_headers_seen` / `expect_final_response` を追加。

## Observable status/details changes (release note 対象)

| ケース | 旧 | 新 |
|---|---|---|
| Compressed-Flag=1 (encoding 宣言なし) | UNIMPLEMENTED "compressed gRPC messages are not supported" | INTERNAL 同文言 |
| Compressed-Flag=1 + 未対応 `grpc-encoding` | UNIMPLEMENTED "unsupported grpc-encoding: ..." | INTERNAL 同文言 |
| 未対応 `grpc-encoding` 宣言 + flag=0 message | UNIMPLEMENTED "unsupported grpc-encoding: ..."（受信前に拒否） | 成功（wire の grpc-status に従う） |
| RST_STREAM `HTTP_1_1_REQUIRED(0xd)` | UNKNOWN | INTERNAL |
| :status 200 + DATA END_STREAM で trailers 欠落 | UNKNOWN details 空 | INTERNAL "server closed the stream without sending trailers" |
| headers-only / trailing HEADERS で grpc-status 欠落 | UNKNOWN | UNKNOWN（変更なし、明示的にテストで固定） |

## Verification

- `tools/test/check-c-unit.sh`: protocol_core / status_core / transport_core すべてpass（2026-07-10）。
- `tools/test/check-phpt.sh`: 17/17 pass。
- PHPUnit 統合スイート: 31 tests / 116 assertions OK。
- 敵対的レビュー対応後（2026-07-10）: C unit 3本 / PHPT 17/17 / PHPUnit 31 tests / C static analysis すべてpass（test-server 再ビルド込み）。
- 敵対的再レビュー対応後（2026-07-10）: 1xx / END_STREAM ゲートのケース追加込みで C unit 3本 / PHPT 17/17 / PHPUnit 31 tests / C static analysis すべてpass。

## Decision Log

- UNIMPLEMENTED → INTERNAL の変更はアプリから見えるコード変更なのでリリースノート記載が必要。
- trailers 欠落判定は `stream_error_code == NGHTTP2_NO_ERROR`（clean END_STREAM）に限定した。RST 由来の close は `stream_reset_seen` 側の taxonomy が先に効き、ローカルエラーによる close（reset frame なしで error_code 非0）は従来どおり UNKNOWN のまま（issue のスコープ「trailers なしで END_STREAM」に厳密に対応）。
- trailers 欠落の details は従来空文字列だったが、INTERNAL の details fallback（"malformed gRPC response frame"）に落ちると誤解を招くため、grpc-go と同文言 "server closed the stream without sending trailers" を新設した（既存文言の変更ではなく追加）。
- missing trailers の分類 policy は「ext-grpc drop-in（UNKNOWN "Stream removed"）」「grpc-go exact」「全 clean-close を strict に INTERNAL」の3択から **grpc-go exact** を採用（2026-07-10）。理由: 本 issue の変更根拠自体が grpc-go の分類であること、ext-grpc の文言は C-core transport エラーの偶発的な表現で仕様意図を表さないこと、全 clean-close INTERNAL は公式のどちらとも一致しない独自 policy になること。
- `grpc-encoding` header は観測のみとし、未対応圧縮の失敗は DATA parser の Compressed-Flag=1 検出時に立てる（grpc-go `checkRecvPayload` は `compressionMade` の場合のみ compressor lookup する）。ext-grpc 1.80.0 実測（レビュー probe）でも gzip 宣言 + flag=0 + status0 は OK。

## Close Criteria

- 3 ケースの C unit テストが INTERNAL を検証して通る。
- 既存 PHPT / PHPUnit スイートに回帰がない。
