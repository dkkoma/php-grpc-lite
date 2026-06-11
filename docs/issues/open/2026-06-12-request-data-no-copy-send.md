---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/request-data-no-copy-send
---

# NGHTTP2_DATA_FLAG_NO_COPY でリクエスト payload の二重 memcpy を排除する

## 目的

リクエスト送信時の「zend_string → nghttp2 フレームバッファ → write coalesce バッファ(または SSL_write)」という 2 段 memcpy を、nghttp2 の no-copy データ供給 API で 1 段以下に削減する。

## 背景

現状の送信データフロー:

1. `data_source_read_callback` (`src/transport.c:1828`) → `copy_request_bytes` (`transport.c:1801`) が PHP から渡された request payload (`call->request` = `ZSTR_VAL(request_payload)` を直接参照) を nghttp2 の内部フレームバッファへ **memcpy (コピー1)**。
2. nghttp2 がフレーム化したチャンクを `send_callback` (`transport.c:1712`) へ渡し、`h2_connection_buffer_or_write` が coalesce バッファへ **memcpy (コピー2)**、または直接 `SSL_write`/`send`。

nghttp2 には `NGHTTP2_DATA_FLAG_NO_COPY` + `nghttp2_session_callbacks_set_send_data_callback` があり、`read_callback` は長さ確定のみ行い、実データは `send_data_callback` がフレームヘッダ(9 byte、nghttp2 から `framehd` で渡される)と source データを自前で書き出す。これにより:

- コピー1 が完全に消える(payload は zend_string から直接 wire へ)。
- 大リクエスト(Spanner の大きい mutation / insert batch 等)では memcpy 量が半減〜2/3 減。

## spec照合

- gRPC spec: Length-Prefixed-Message の 5 byte prefix (`call->grpc_header`) も `copy_request_bytes` が供給している。`send_data_callback` 実装では「framehd 9B → (必要なら) grpc_header 残り → payload 残り」の順で書く必要がある。フレーム境界が 5 byte prefix をまたぐケース(初回フレーム)を `request_offset` ベースで正しく扱うこと。
- RFC 9113: DATA フレームのヘッダ + payload を連続して書くだけであり、wire format は不変。padding は使っていないので `frame->data.padlen == 0` 前提で良い(0 でない場合は `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` を返して通常経路へフォールバック可能)。

## 修正方法

1. `configure_callbacks` (`transport.c:338`) に `nghttp2_session_callbacks_set_send_data_callback` を追加。
2. `data_source_read_callback` を NO_COPY 版に変更: `*data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;` を立て、buf への memcpy をやめて長さ(min(remaining, length))だけ返す。EOF 判定は現行ロジックを流用。
3. `send_data_callback` 実装:
   - `h2_connection_buffer_or_write(framehd, 9)` → grpc_header の未送信分 → `call->request + payload_offset` の順に書き、`call->request_offset` を進める。
   - coalesce バッファ有効時は小フレームはまとまり、大フレームは直接 write される(既存ロジック流用)。
   - 失敗時は `NGHTTP2_ERR_CALLBACK_FAILURE` を返す(接続 dead 化の既存処理に乗る)。
4. trace (`grpc_lite_trace_outbound_frame`) は send_callback 経由でなくなる DATA フレーム分の記録を `send_data_callback` 側にも追加する。
5. 注意: `call->request` は unary では `call->request_payload`(Call オブジェクト保持)、streaming では `state->request` が生存を保証しており、stream 完了まで解放されないことを確認済み。所有権の変更は不要。

## 期待効果

- 大リクエスト送信の memcpy をほぼ半減。小リクエストでも nghttp2 内部バッファ確保・コピーの固定費が減る。

## 完了条件

- DATA payload が zend_string から coalesce バッファ/socket へ直接書かれている。
- 16KB 境界をまたぐ payload、5 byte prefix がフレーム境界をまたぐケース、空 payload (`request_len == 0`) の PHPT が通る。
- 既存テスト全通過、送信ベンチで悪化なし。

## 測定ベンチマーク

- 主計測: `./bench/run.sh upload-unary` / `./bench/run.sh tls-upload-unary` — リクエスト payload sweep(1KB〜4MB)。`upload_unary_1048576b` / `4194304b` の `wall_time_ns_per_call` で memcpy 削減の効果を見る。`upload_unary_1024b` で小 payload の固定費悪化がないことも確認。
- 回帰確認: `./bench/run.sh cpu-micro` / `tls-cpu-micro`(callback 配線変更による小 RPC 固定費の悪化なし)、`./bench/run.sh spanner-shape`。
- 前提: #4 (coalesce-capacity) 適用後の状態を before として計測する(send_data_callback は coalesce 構造に依存するため)。

## Progress

## Verification

## Decision Log
