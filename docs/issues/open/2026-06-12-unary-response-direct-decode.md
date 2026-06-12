---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/unary-response-direct-decode
Related: docs/issues/open/2026-05-14-response-delivery-hotpath.md
---

# unary response を direct decode 経路に乗せて二重パースと余分なコピーを排除する

## 目的

unary call のレスポンス受信で発生している「gRPC message framing の二重パース + payload の二重コピー(+smart_str 成長 realloc)」を、server streaming で実装済みの direct decode 経路を再利用して排除する。

## 背景

現状の unary 経路 (`grpc_lite_unary_call_perform_core_on_connection`, `src/unary_call.c:53`) のデータフロー:

1. `on_data_chunk_recv_callback` (`src/transport.c:1864`) — unary では `direct_response_payload` が false のため:
   - `grpc_protocol_validate_response_message_lengths` (`transport.c:2493`) が 5 byte prefix を **パースして検証**(コピーなし、ただし全 chunk を走査)
   - `smart_str_appendl(&call->body, data, len)` で **wire bytes 全体(5 byte prefix 込み)をコピー** (コピー1)。大きいレスポンスでは smart_str の成長 realloc も発生。
2. `typed_result->body = zend_string_copy(call.body.s)` (`unary_call.c:192`) — refcount のみ。
3. `grpc_lite_extract_unary_payload` (`src/wrapper_adapter.c:421`) — body を **再パース**し、payload を `zend_string_init` で **新規 zend_string にコピー** (コピー2)。

つまり N byte のレスポンスに対して、kernel→recv_buf 以降に約 2N byte の memcpy + framing 2回パース + smart_str realloc が発生する。

一方 server streaming は `grpc_protocol_process_response_data_direct` (`transport.c:2578`) で 5 byte prefix を読んだ時点で `zend_string_alloc(payload_len)` し、DATA chunk から **最終 zend_string へ直接 memcpy(コピー1回のみ)** している。unary でもこの経路をそのまま使える。

## spec照合

- gRPC over HTTP/2 spec: unary response は Length-Prefixed-Message ちょうど 1 個 + trailers。現行 unary は `max_response_messages = 1` で 2 個目を `malformed_response_frame` として弾いており、direct 経路 (`grpc_protocol_process_response_data_direct`) にも同一の `max_response_messages` チェック・compressed-flag (`response_header_buf[0] > 1` / `== 1`)・`max_receive_message_bytes` チェックがあるため、検証セマンティクスは等価。
- 注意: `grpc_lite_extract_unary_payload` が担っていた「body 全長 = 5 + payload_len の整合チェック」(`wrapper_adapter.c:440`) は、direct 経路では `server_streaming_call_next_resource_core` 相当の「残骸チェック」(`response_header_len != 0 || response_payload != NULL` → malformed, `src/server_streaming_call.c:360`) を stream close 後に行うことで置き換える必要がある。

## 修正方法

1. `unary_call.c` で `call.decode_response_incrementally = true; call.direct_response_payload = true; call.queue_response_payloads = true;` を設定し、`max_response_messages = 1` を維持。
2. 受信ループ終了後、`call.response_queue_head` から payload を 1 個取り出して `typed_result->body` 相当として返す(`smart_str body` は unary では未使用化)。queue が空なら現行の「OK なのに body なし → INTERNAL」分岐へ。
3. stream close 後に `response_header_len != 0 || response_payload != NULL || response_payload_offset != 0` なら `malformed_response_frame = true`(truncated message 検出)。
4. `grpc_lite_extract_unary_payload` の prefix 剥がしを削除し、`grpc_lite_perform_call_unary` (`wrapper_adapter.c:465`) では payload zend_string をそのまま `call->unary_response_payload` へ move(`zend_string_copy` ではなく所有権移動でコピーゼロ)。
5. read-ahead 制限 (`server_streaming_read_ahead_limit_would_exceed`) は `current_read_call == call` の自ストリーム受信では発動しないため unary に影響しないことをテストで確認。
6. BENCH ビルドの `body` 出力 (`unary_call.c:10`) は payload ベースに追従させる。

## 期待効果

- 大レスポンス(数百 KB〜数 MB の Spanner ResultSet 等)で memcpy 量を約半減、smart_str realloc を排除。
- framing パースが 1 回になる。

## 完了条件

- unary のレスポンス payload が DATA chunk から最終 zend_string へ 1 回の memcpy で到達している。
- 既存 PHPT(malformed frame / compressed / too large / 複数 message / 空 message / trailers-only)が全部通る。
- 下記ベンチで大 payload の改善を確認し、小 payload で悪化がないこと。

## 測定ベンチマーク

- 主計測: `./bench/run.sh payload-unary` / `./bench/run.sh tls-payload-unary` — レスポンス payload sweep(デフォルト 0〜4MB)。特に `payload_unary_1048576b` / `payload_unary_4194304b` の `wall_time_ns_per_call` と p50 で memcpy 半減の効果を見る。`payload_unary_0b` / `100b` で固定費の悪化がないことも確認。
- 回帰確認: `./bench/run.sh spanner-shape`(現実的な shape)、`./bench/run.sh metadata-header`(trailers 経路)。
- 比較: `./bench/compare.sh payload-unary` で ext-grpc との相対位置の変化も記録。

## Progress

- 2026-06-12: 実装完了(branch は #3 perf/unary-recv-buffer-size の上に積む stacked 構成。同じ受信ループを触るため)。
  - `unary_call.c`: `decode_response_incrementally / direct_response_payload / queue_response_payloads = true`(`max_response_messages = 1` 維持)。受信ループ後に `grpc_lite_unary_take_response_payload()` で queue から payload を 1 個 pop し `typed_result->body` へ所有権移動。`smart_str body` は unary では未使用化。
  - truncated 検出: stream close 後に `response_header_len != 0 || response_payload != NULL || response_payload_offset != 0` なら `malformed_response_frame = true`(streaming の残骸チェックと同型)。
  - `wrapper_adapter.c`: `grpc_lite_extract_unary_payload` を削除し、`result.body`(= decoded payload)を `call->unary_response_payload` へ move(コピーゼロ)。OK + message なしは従来どおり OK + null message(旧実装でも empty body → payload NULL で OK のままだった。issue 記載の「OK なのに body なし → INTERNAL」は旧コードでは到達しない分岐だった点を注記)。
  - BENCH diagnostic の `body` は queue 先頭 payload ベースに追従。
  - エッジ固定化: test-server raw fixture (50054) に `x-bench-grpc-response: two-messages` knob を追加し、022 PHPT に「2 message 目 → INTERNAL malformed」「空 message + grpc-status 0 → OK + 空 payload message」を追加。truncated (`partial-frame`)・compressed・invalid content-type・unsupported encoding は既存 022 でカバー。
  - read-ahead 制限は `current_read_call == call` の自ストリームでは発動しない(`server_streaming_read_ahead_limit_would_exceed` が `current_read_call != call` を要求)ため unary に影響しない。
- 関連 issue [response-delivery-hotpath](2026-05-14-response-delivery-hotpath.md) の unary 側スコープは本 issue で実装済み。

## Verification

- PHPT 15/15(022 に追加した unary エッジ含む)、C unit、静的解析、PHPUnit 30/30 すべて PASS。
- ベンチ(before: #3 適用後 `recv-buffer-after-20260612` / after: `direct-decode-after-20260612`、p50):
  - payload-unary 1MB: 497.9 → 477.1µs、**4MB: 2205.9 → 1482.6µs(-33%)**
  - tls-payload-unary 1MB: 847.2 → 769.6µs(-9%)、**4MB: 3324.7 → 2413.1µs(-27%)**
  - 小 payload(0b/100b/10KB): 揺れ幅内で悪化なし(10KB plain 28.2→27.7µs)
  - 回帰: spanner-shape p50 28.3〜31.5µs、metadata-header req50+resp50 p50 160.3µs — いずれも main 計測分布内。

## Decision Log

- 2026-06-12: **採用**。大 payload で memcpy 半減 + smart_str realloc 排除 + framing 1回化の効果が 4MB で -27〜-33% と明確。検証セマンティクスは direct 経路の同一チェック(compressed flag / max_receive_message_bytes / max_response_messages)+ stream close 後の残骸チェックで等価以上をテストで固定化。
