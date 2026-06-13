# unary response direct decode 切り替え (perf/unary-response-direct-decode) HTTP/2/gRPC ドメインモデルレビュー 2026-06-12

## Scope

- `src/unary_call.c` (direct decode flags 設定、stream close 後の residue check、`grpc_lite_unary_take_response_payload()`)
- `src/wrapper_adapter.c` (`grpc_lite_extract_unary_payload` 削除、`result.body` の所有権移動)
- `src/transport.c` (`grpc_protocol_process_response_data_direct()` / `grpc_protocol_validate_response_message_lengths()` / `enqueue_response_payload()` / `free_queued_response_payloads()` / `server_streaming_read_ahead_limit_would_exceed()` — 既存コードの参照経路として)
- `src/status_core.c` (status priority の参照)
- `poc/test-server/main.go` (`two-messages` knob), `tests/phpt/022-error-and-http-validation.phpt`
- 対象 diff: commit ec81535 (`git show HEAD`)

## Reviewer Role

- `HTTP/2 / gRPC domain model reviewer` (docs/verification/protocol-model-review-guide.md 準拠)

## Review Prompt Summary

- unary response を server streaming と同じ direct-decode 経路 (`decode_response_incrementally / direct_response_payload / queue_response_payloads`, `max_response_messages = 1`) に切り替える変更について、(1) gRPC over HTTP/2 の unary semantics (exactly one Length-Prefixed-Message / trailers-only / empty message / truncated / compressed flag / max_receive_message_bytes / duplicate messages) が旧 `validate_response_message_lengths` + smart_str + `grpc_lite_extract_unary_payload` 経路と同等以上に検出されるか、(2) direct 経路が `discard_response_body` を参照しないことの unary への帰結 (invalid content-type / unsupported grpc-encoding / DATA after trailers)、(3) queue entry の lifecycle と所有権 (leak / double free)、(4) stream_closed なしで loop を抜けた場合の status 解決 (DEADLINE_EXCEEDED / UNAVAILABLE 優先)、(5) read-ahead limit が unary で誤発火しないこと、を確認する。

## 検証結果 (指摘に至らなかった確認事項)

1. **Unary spec semantics の同等性 (Q1)**: 主要 case はすべて旧経路と同等または改善。
   - *exactly one message*: `max_response_messages = 1` は旧経路 (`transport.c:2539`) と direct 経路 (`transport.c:2624`) の両方で 5-byte prefix 確定時に count check されるため、2 個目 (empty message 含む) は `malformed_response_frame` + RST_STREAM(CANCEL) → INTERNAL で同一。新規 `two-messages` fixture と 022 PHPT で初めてテスト固定された (改善)。
   - *trailers-only*: HEADERS(END_STREAM) + `grpc-status` は `on_frame_recv_callback` の `grpc_status_seen && initial_headers_end_stream` gate (`transport.c:2041`) で invalid_content_type を回避し、queue 空 → `typed_result->body == NULL` → server status がそのまま返る。旧経路 (body 空 → payload NULL) と同一。
   - *empty message*: direct 経路は `zend_string_alloc(0)` を即 enqueue (`transport.c:2673-2676, 2688-2703`)、旧経路は extract が空 payload を返す。どちらも「非 NULL の空 payload + OK」。022 PHPT で固定 (改善)。
   - *truncated message*: 旧経路は wrapper の re-parse (`payload_len + 5 != body_len` → INTERNAL、ただし status OK 時のみ) で検出。新経路は residue check (`unary_call.c:220-226`) で `stream_closed` 時に検出し、grpc-status 0 の trailers + 途中で切れた body も INTERNAL になる。trailers 完了後の不完全 message は framing violation なので spec 上も INTERNAL が妥当 (同等以上)。RST による truncation は REVIEW-20260612-001 参照。
   - *compressed flag*: `compressed_response_seen` → UNIMPLEMENTED + RST(CANCEL)、residue check の gate (`!compressed_response_seen`) で malformed への上書きなし。同等 (check 順序の差は REVIEW-20260612-003)。
   - *max_receive_message_bytes*: `response_message_too_large` → RESOURCE_EXHAUSTED + RST(CANCEL)、residue gate あり。同等。
   - *DATA after trailers*: 両経路とも入口で `grpc_status_seen` → `invalid_grpc_status` + RST(CANCEL) → UNKNOWN (`status_core.c:14`)。同一コード。
2. **メモリ/所有権 (Q3)**: leak / double free なし。
   - `grpc_lite_unary_take_response_payload()` は entry を pop して `efree(entry)` し payload 所有権を `typed_result->body` へ移動。`grpc_lite_unary_result_dtor()` (`unary_call.c:257-267`) は body 未移動時に release、wrapper (`wrapper_adapter.c:535-541`) は OK 時のみ `call->unary_response_payload` へ move して `result.body = NULL` にするため、非 OK status で enqueue 済みの payload も dtor で確実に解放される。
   - 部分受信中の `call->response_payload` (timeout / connection error での早期 break) は `cleanup_grpc_call()` (`transport.c:2902-2930`) が release。queue 残余は `free_queued_response_payloads()` が解放。pop 済み entry は queue から外れているので double free なし。
   - BENCH diagnostic 経路 (`typed_result == NULL`) は payload を queue に残したまま `zend_string_copy` で参照し (`unary_call.c:10`)、`cleanup_grpc_call()` が queue を解放。`typed_result` と `diagnostic_result` は呼び出し側で排他 (`unary_call.c:253, 273`) のため、「take 後に diagnostic が空 body を見る」順序問題も実害なし。
3. **stream_closed なしの loop 終了 (Q4)**: residue check は `call.stream_closed` で gate されるため、deadline 超過 (`timed_out`) や connection error (EOF / recv 失敗) では `malformed_response_frame` は立たない。status 解決は `timed_out` が最優先 (`status_core.c:12`) で DEADLINE_EXCEEDED、response 前 EOF は `http_status == -1` → UNAVAILABLE (`status_core.c:27`)、HEADERS 受信後 mid-body EOF は UNKNOWN。いずれも旧 unary 経路 (re-parse は status OK 時のみ実行) と同一。仮に residue が立っても `timed_out` の priority が上のため DEADLINE_EXCEEDED は崩れない。
4. **read-ahead limit (Q5)**: `server_streaming_read_ahead_limit_would_exceed()` (`transport.c:2710-2722`) は `current_read_call != call` が必須条件。unary は `nghttp2_session_mem_recv` の前後で `connection->current_read_call = &call` を set/clear (`unary_call.c:199-201`) するため自 stream の DATA では発火しない。逆方向 (別 call の read loop 中に unary stream へ DATA が届く) は構造的に不可能: unary call は `grpc_lite_unary_call_perform_core_on_connection()` 内で同期的に submit→受信→`clear_connection_call_owner()`(`unary_call.c:241`、全 early-return path 含む) まで完結し、関数を出る時点で stream user data は必ず unregister 済み。preflight drain (`current_read_call == NULL`、`transport.c:174` の ZEND_ASSERT) は unary submit 前にしか走らず、その時点で unary stream は存在しない。したがって unary が `response_queue_limit_exceeded` (誤った "server streaming read-ahead queue limit exceeded" details) に到達する経路はない。
5. **責務配置**: 5-byte prefix の re-parse が wrapper adapter (PHP 層) から消え、gRPC framing の解釈が gRPC protocol 層 (`process_response_data_direct`) に一本化された。`grpc_lite_unary_take_response_payload()` は queue (gRPC Call state) からの pop のみで protocol 解釈を持たず、層分離はむしろ改善。

## Issues

### REVIEW-20260612-001: residue check が RST_STREAM による異常終了の truncation を malformed_response_frame に畳み、error taxonomy を退行させる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `unary_call.c:220-226` の residue check は `stream_closed` かつ message 途中残骸があれば無条件に `malformed_response_frame = true` とするが、server が message 送信途中に RST_STREAM(CANCEL) で abort した場合、truncation は protocol violation ではなく stream abort の帰結。`status_core.c` の priority では `malformed_response_frame` (line 32) が `stream_reset_seen` の RST code mapping (line 36-58) より先に評価されるため、旧 unary 経路で CANCELLED (NGHTTP2_CANCEL) / RESOURCE_EXHAUSTED (ENHANCE_YOUR_CALM) になっていた case が INTERNAL "malformed gRPC response frame" に変わる。
- Evidence: `src/unary_call.c:220-226`、`src/status_core.c:32-58`。旧経路は residue 検出が `grpc_lite_extract_unary_payload` (status OK 時のみ実行) だったため、RST mid-message では server/RST 由来の status がそのまま返っていた。なお同じ residue logic は `src/server_streaming_call.c:358-359` に既存 (本 commit 以前から) であり、streaming も同じ taxonomy を持つ。
- Expected model: ガイド §7 の error taxonomy で、RST_STREAM は stream-local failure として error code mapping (CANCELLED / UNAVAILABLE / RESOURCE_EXHAUSTED 等) に従う。malformed gRPC frame (INTERNAL) は「server が framing 規約に違反した」場合 (trailers 受信後に message が不完全、flag byte 不正など) に限るべきで、abort による不完全 message は framing violation ではない。gRPC C-core も RST mid-message では RST 由来 status を返す。
- Why it matters: CANCELLED と INTERNAL では呼び出し側 (retry policy / 監視分類) の扱いが異なる。server 側 deadline propagation による cancel は実運用で発生する。
- Recommended fix: residue check の gate に `!call.stream_reset_seen` を追加する (`stream_closed && !response_message_too_large && !compressed_response_seen && !stream_reset_seen && (...)`)。trailers 完了後の truncation (RST なし) は引き続き INTERNAL のまま。`server_streaming_call.c:358` にも同じ gate を入れて unary / streaming の taxonomy を揃える。
- 必要なテスト: test-server に「partial message → RST_STREAM(CANCEL)」fixture を追加し、unary / streaming とも CANCELLED を固定する。
- Fix summary: residue check の gate に `!stream_reset_seen` を追加(`unary_call.c` / `server_streaming_call.c` 両方)。test-server に `x-bench-grpc-response: partial-frame-abort` fixture(partial DATA flush 後に `panic(http.ErrAbortHandler)` → RST_STREAM(INTERNAL_ERROR))を追加し、022 PHPT で details が "HTTP/2 stream reset: 2"(malformed ではない)になることを固定。Go の h2 handler から RST(CANCEL) を直接生成できないため INTERNAL_ERROR で taxonomy gate を固定する。
- Fix commit: perf/unary-response-direct-decode(レビュー反映コミット)
- Verification: PHPT 15/15 PASS(再ビルド後)
- Notes: 「非 OK trailers + truncated body」が server status ではなく INTERNAL になる変更点も確認したが、こちらは trailers 後に message が完結していないこと自体が framing violation のため INTERNAL を妥当と判断 (旧 unary より厳密化、streaming と一致)。

### REVIEW-20260612-002: direct decode 経路が discard_response_body を一切参照せず、flag が write-only になった

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `grpc_protocol_process_response_data_direct()` (`transport.c:2596-2708`) は loop 入口でも enqueue 時でも `discard_response_body` を見ない。unary も streaming も direct 経路に揃った結果、`discard_response_body` の read は dead path の `transport.c:1906` のみとなり、header 検証 (invalid content-type `transport.c:1974`、unsupported grpc-encoding `transport.c:1983`、initial HEADERS の不正 grpc-status `transport.c:2043,2047`、metadata too large `transport.c:2789`) が flag を set しても DATA は decode・allocate・enqueue され続ける。
- Evidence: `src/transport.c:2596-2708` (read なし)、`src/transport.c:1896-1901` (direct 分岐は discard check の手前で return)、set 箇所は上記。
- Expected model: 「response が header 段階で invalid と確定したら body buffering を止める」という discard は gRPC protocol 層の明示的 state。set する層と consume する層が一致していなければモデルとして死んでいる。
- Why it matters: **PHP-visible な status / payload の差異はないことを確認済み** — discard を set する全経路は status priority 上 `grpc_status` より先に評価される flag (invalid_content_type → UNKNOWN, unsupported_response_encoding → UNIMPLEMENTED, metadata_too_large → RESOURCE_EXHAUSTED, invalid_grpc_status → UNKNOWN) を必ず伴うため status は OK にならず、wrapper は payload を採用しない (`wrapper_adapter.c:535`)。queue 済み payload も result dtor / `cleanup_grpc_call()` で解放され leak しない。実害は「破棄確定 response のための payload allocation + copy」(unary は `max_response_messages = 1` で最大 1 message ≤ max_receive_message_bytes に有界) と、flag の意味が崩れた将来リスク (新たな discard 経路を足しても direct 経路が無視する) に限られる。
- Recommended fix: `grpc_protocol_process_response_data_direct()` の入口 (grpc_status_seen check の直後) に `if (call->discard_response_body) { return 0; }` を追加する。これで header 検証経路の RST 前後に届く DATA の decode を止め、flag の set/consume が再び対になる。
- 必要なテスト: 022 PHPT の invalid content-type / unsupported encoding fixture で status・details が不変であることを確認 (既存 assert で十分)。
- Fix summary: `grpc_protocol_process_response_data_direct()` の grpc_status_seen check 直後に `if (call->discard_response_body) return 0;` を追加し、set/consume を対に戻した。
- Fix commit: perf/unary-response-direct-decode(レビュー反映コミット)
- Verification: PHPT 15/15 PASS(022 の invalid content-type / unsupported encoding assert 含む)
- Notes: streaming 側は本 commit 以前から同じ挙動 (`server_streaming_call.c:265` の loop 条件と line 322 の terminate で flag 検知後は recv を止めるため、消費側でも payload が PHP に渡る前に status 化される)。

### REVIEW-20260612-003: 旧 validate + smart_str body 経路が到達不能になり、check 順序の差異 (compressed × too-large) だけが behavior change として残った

- Severity: `Low`
- Status: `Fixed`(軽量対応。parser 一本化は別リファクタ issue へ)
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: 本 commit で unary も direct 経路に揃ったため、`on_data_chunk_recv_callback` の `grpc_protocol_validate_response_message_lengths()` + `smart_str_appendl(&call->body, …)` 分岐 (`transport.c:1903-1910`) を通る call 生成経路が存在しなくなった (unary / streaming とも 3 flag を set)。一方で両 parser は check 順序が異なる: 旧は flag byte (>1 → malformed, ==1 → compressed) を length 上限より先に判定 (`transport.c:2547-2570`)、direct は length 上限を先に判定 (`transport.c:2635-2666`)。このため「compressed-flag = 1 かつ length > max_receive_message_bytes」の最初の message は、旧 unary では UNIMPLEMENTED (compressed)、新 unary では RESOURCE_EXHAUSTED (too large) になる。
- Evidence: `src/transport.c:1896-1910, 2511-2594, 2596-2708`、`src/status_core.c:31,33` (RESOURCE_EXHAUSTED が UNIMPLEMENTED より優先)。
- Expected model: gRPC frame parser は 1 つであるべきで、2 実装が併存すると check 順序のような semantics が silent に分岐する (現にした)。unary が streaming と同じ順序になったこと自体は call type 間の一貫性として正しい方向。
- Why it matters: dead code の保守コストと、将来 flag 組み合わせを変えた際に検証の弱い旧 parser へ落ちる事故リスク。status の差異自体は「両方とも error で、どちらの分類も spec 上 defensible」な edge (compressed 未対応 client に compressed かつ巨大な message) のため実害は小さい。
- Recommended fix: 旧経路 (`validate_response_message_lengths` + `smart_str body` field + `decode_response_incrementally / direct_response_payload / queue_response_payloads` の 3-flag 分岐) を削除し、direct 経路を唯一の parser にする。3 つの bool は常に同時 set なので単一 mode に畳む。別 perf issue と衝突するなら、少なくとも `grpc_call.body` が unary で不使用になったことを docs/internal comment に明記する。
- 必要なテスト: 削除時に既存 022 / streaming PHPT が全通過することを確認。compressed × oversized の順序は fixture 化できるなら RESOURCE_EXHAUSTED で固定。
- Fix summary: 軽量対応を採用: `grpc_exchange_state.h` の `smart_str body` に「production の unary / streaming は direct decode 経路を使い、legacy 経路は BENCH diagnostic raw client のみが使用」と明記。`validate_response_message_lengths` は `src/diagnostic/bench.c:270` が現役で使用しているため削除は本 issue では行わず、parser 一本化(3-flag の単一 mode 化含む)は別途リファクタ issue とする。
- Fix commit: perf/unary-response-direct-decode(レビュー反映コミット)
- Verification: 既存テスト全通過
- Notes: BENCH diagnostic の `body` field も raw wire bytes (5-byte prefix 付き) から decoded payload (queue 先頭) へ意味が変わったが、repo 内に `['body']` を読む bench/PHPT consumer は見つからなかったため指摘は本 issue の付記に留める。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `2`
- Low: `1`
- Design Decision: `none`
