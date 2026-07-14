# -bin メタデータ送信をパディングなし base64 にする

- Status: Closed
- Created: 2026-07-08
- Branch: codex/issue-bin-metadata-unpadded-base64
- Owner: Claude

## Background

仕様は Binary-Header の base64 について「送信はパディングなしを推奨、受信は両方受理必須」と定める。

> Binary-Header → {Header-Name "-bin" } {base64 encoded value}
> ...implementations MUST accept padded and un-padded values and should emit un-padded values.
> — [PROTOCOL-HTTP2.md § Requests](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#requests)

現状の実装は `append_custom_request_header_value`（`src/transport.c`）で `php_base64_encode` の**パディング付き**出力をそのまま送出している。受信側（`add_binary_metadata_values_to_map`、同ファイル）は `php_base64_decode` がパディング有無どちらも受理するため準拠済み。カンマ結合値の分割も実装済み。

should 違反であり実害は限定的だが、公式実装は両方ともパディングなしで送出しており、厳格なサーバー／プロキシ実装との interop で差が出得る。

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: chttp2 の base64 エンコーダはパディングなしで出力する。
  - 実装: [bin_encoder.cc `grpc_chttp2_base64_encode`](https://github.com/grpc/grpc/blob/master/src/core/ext/transport/chttp2/transport/bin_encoder.cc) — `static const uint8_t tail_xtra[3] = {0, 2, 3};`（パディングありなら末尾は常に 4 バイトになるところ、2/3 バイト＝ `=` なし）
- **Go 公式 (grpc-go)**: エンコードは `base64.RawStdEncoding`（パディングなし）、デコードは長さが 4 の倍数なら `StdEncoding`、それ以外は `RawStdEncoding` で両対応。
  - 実装: [internal/transport/http_util.go `encodeBinHeader` / `decodeBinHeader`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http_util.go)

## Goals

- `-bin` メタデータ送出時に base64 の `=` パディングを除去する。

## Non-Goals

- 受信側の変更（既に両対応）。

## Plan

- `append_custom_request_header_value` で `php_base64_encode` 後に末尾 `=` を切り詰める（zend_string の len 調整）。
- 単体テスト: 1/2/3 バイト境界の値で `=` が出ないこと、既存の受信テストが引き続き通ること。

## Progress

- 2026-07-10: `append_custom_request_header_value`（`src/transport.c`）で `php_base64_encode` 後に末尾 `=` を切り詰めるよう修正。zend_string の len を調整し NUL 終端を再設定。
- 2026-07-10: `tests/phpt/032-bin-metadata-unpadded-base64.phpt` を追加。1/2/3 バイト境界の値（パディング 2/1/0 個のケース）でワイヤ上の値に `=` が含まれないことを `GRPC_LITE_TRACE_FILE` の `wire.request_header` レコードで検証し、fixture サーバーのエコーで round-trip も確認。
- 2026-07-15: PR #27 マージ（merge commit 68c4010）を確認して Closed。

## Verification

- `tools/test/check-phpt.sh`: 17/17 PASS（新規 032 を含む。既存の 020/023 のメタデータ round-trip も通過 → grpc-go サーバーがパディングなし値を受理）。
- `tests/Integration/MetadataCompatibilityTest.php`: 4 tests / 18 assertions OK。

## Decision Log

- 検証はワイヤトレース（`wire.request_header` の `value`）で行う。`-bin` ヘッダは sensitive 扱いではないため値がそのまま記録される。grpc-go 側の metadata はデコード済みでパディング有無を観測できないため、エコーは round-trip 確認のみに使う。

## Close Criteria

- ワイヤ上の `-bin` ヘッダ値にパディングが含まれないことをトレースまたは Go テストサーバーのエコーで確認。
