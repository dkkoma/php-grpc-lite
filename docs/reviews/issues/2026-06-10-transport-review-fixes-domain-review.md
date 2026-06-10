# transport review fixes (trace/coalesce/direct-decode/header-cap/cache-leak) HTTP/2・gRPC domain model review 2026-06-10

## Scope

- branch `codex/simd-vectorization-audit` の直近 5 commits (`2f98554..b7e17fd`)
  - `5b9e499` src/transport.c — outbound HEADERS frame trace から HPACK block hex dump を削除
  - `5debb54` src/transport.c — `send_pending_h2_frames_with_deadline` で coalesce flush 失敗時に `mark_connection_dead`
  - `2d331d5` src/transport.c, src/grpc_exchange_state.h, src/diagnostic/bench.c — direct decode 経路の到達不能な compressed message 処理と `response_current_compressed` field を削除
  - `c693410` src/transport.c, src/transport_core.h — `grow_request_headers` hard cap の off-by-one 修正 (`+7` → `GRPC_LITE_REQUEST_FIXED_HEADER_COUNT` = 8)
  - `b7e17fd` src/transport.c — `release_persistent_connection_entry_if_mismatched` 追加、cache entry / connection のリーク修正

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer (docs/verification/protocol-model-review-guide.md 準拠)

## Review Prompt Summary

- 命名・責務分離・connection/stream/call/channel scope、flow-control・metadata・status・deadline semantics、RST_STREAM/GOAWAY/EOF lifecycle、persistent connection ownership invariant (`stream_owner_count` / `detached_from_cache` / cache entry lifecycle)、production/bench 境界を 5 commits に対して確認。特に commit 5 の double-free / 他参照中 connection の破棄可否、commit 2 の dead 化条件の正しさと十分性、commit 3 の到達不能性、commit 4 の fixed header count = 8 の妥当性。

## Issues

### REVIEW-20260610-001: persistent cache の key 削除が mismatched entry の健全な connection を巻き添え破棄する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: `remove_unusable_persistent_connection` / `discard_persistent_connection` (src/transport.c:994, 1624) は `entry->connection == connection` を確認する前に `zend_hash_str_del` で cache slot を無条件に削除する。commit `b7e17fd` の `release_persistent_connection_entry_if_mismatched` (src/transport.c:978) はこの mismatch 時に「entry とその connection をリークさせず破棄する」ことで leak を直したが、mismatched entry が保持しているのは「除去対象とは別の、同じ channel identity key で後から cache された connection」であり、それが healthy であっても `detached_from_cache = true` + (unowned なら) `destroy_h2_connection` で evict される。
- Evidence: `remove_unusable_persistent_connection` は src/transport.c:1003 で先に `zend_hash_str_del` を実行。mismatch が成立しうるのは、stale な connection pointer を保持したまま同一 key の slot が新 connection で置き換わった後に除去経路へ入るケース (reentrancy: call credentials の PHP callback 中の別 RPC など)。`get_persistent_connection` 内の呼び出し (src/transport.c:1584/1589) は常に `entry->connection == connection` なので mismatch しない。surface.c:581 (`Channel::close`) は entry から connection を取るため同様に mismatch しない。
- Expected model: Transport Cache の責務は「指定された connection を cache から外す」こと。slot が別 connection を保持しているなら、その slot は除去対象 connection とは無関係であり、cache から削除すべきではない (guide §3: Transport Cache は per-call state を持たず、connection entry を channel identity key で保持する)。
- Why it matters: memory safety は保たれる (`stream_owner_count > 0` なら破棄は owner 解放まで遅延され、double-free も発生しない — 後述の Verification 参照) が、stale pointer の除去という stream/connection-local な操作が、再利用可能な healthy connection の eviction という cache-global な副作用を持つ。persistent connection の reuse 率を不必要に下げ、TLS handshake の再実行を招く。
- Recommended fix: hash からの削除を `entry != NULL && entry->connection == connection` の場合に限定する (`zend_hash_str_del` の前に lookup 結果を比較)。mismatch 時は slot に触れず return する。これにより `release_persistent_connection_entry_if_mismatched` 自体が不要になる。
- Fix summary: `remove_unusable_persistent_connection` / `discard_persistent_connection` を lookup-first に変更し、`zend_hash_str_del` は `entry->connection == connection` の場合のみ実行。mismatch (slot が空 or 別 connection 保持) 時は cache に触れず、渡された connection を detached 規約 (`detached_from_cache = true` + `destroy_detached_connection_if_unowned`) で解放。`release_persistent_connection_entry_if_mismatched` helper は削除。`discard_persistent_connection` に `connection == NULL` の early return を追加。
- Fix commit: `bfcf5a6`
- Verification: PHPT 15/15 pass、C unit tests pass、static analysis pass。再レビュー (re-review) で以下を確認: (1) 全 caller の整合 — `get_persistent_connection` (src/transport.c:1572/1577/1582) は `entry->connection` を渡すため常に match、surface.c:581 (`Channel::close`) は直前 lookup の `entry->connection` を渡すため match (`entry->connection` は create 時に設定され NULL になる経路なし、新設の NULL early return は防御のみで既存 caller から到達不能 — NULL を渡す caller は存在せず旧 NULL-evict 挙動に利用者なし)、wrapper_adapter.c:550 / bench.c:1850,1857 は使用した connection を渡し、reentrancy で slot が置換済みの場合のみ mismatch → healthy な replacement を温存しつつ stale connection をリークなく解放 (本修正の意図どおり)。(2) ownership invariant — match + `stream_owner_count > 0` は entry のみ破棄 + `detached_from_cache` で最終 owner の解放まで destroy 遅延、match + unowned は entry と connection を破棄、mismatch は unowned のときのみ destroy。各経路で entry の破棄は高々 1 回、double-free / UAF 経路なし (hash table 自体は dtor を持たず明示破棄方式のため `zend_hash_str_del` との二重解放もなし)。
- Notes: この over-eviction 自体は commit 以前から存在 (旧コードは leak)。commit 5 は「leak → 安全な破棄」への改善であり退行ではない。mismatch の到達経路が狭い (reentrancy 必須) ため Medium とする。

### REVIEW-20260610-002: write 失敗時の connection dead 化が関数内で完結していない (large-frame bypass の partial write)

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: commit `5debb54` は `send_pending_h2_frames_with_deadline` (src/transport.c:1769) で `rv != 0 && write_buffer_len > 0` のとき `mark_connection_dead` するが、これは coalesce buffer に残った bytes の破棄ケースのみを覆う。`h2_connection_buffer_or_write` の large-frame bypass 経路 (src/transport.c:1375-1379: `length > GRPC_LITE_H2_WRITE_COALESCE_CAPACITY` で flush 後に直接 `h2_connection_write_all`) や非 TLS の直接 write では、`h2_connection_write_all` が部分書き込み後に失敗すると `write_buffer_len == 0` のまま wire に partial frame が残り、nghttp2 は当該 frame を未送信扱いするため、connection を再利用すると同一 bytes が再送されて wire が壊れる。この場合、関数内の guard では dead 化されない。
- Evidence: src/transport.c:1341-1352 (`h2_connection_write_all` は途中失敗で部分送信済みでも FAILURE のみ返す)、src/transport.c:1769-1774 (guard 条件)、コメントは "the connection must never be reused" と関数内での完結を示唆。
- Expected model: write 失敗は session state と wire の divergence を意味しうるため connection failure として一律 dead 化する (guide §7: connection failure は dead/draining にして cache から外す)。現状は呼び出し側全箇所 (unary_call.c:142/181, server_streaming_call.c:16/172/278/316, transport.c:889-901, create_h2_connection の setup 失敗時 destroy) が `rv != 0` で `mark_connection_dead` または破棄しているため実害はないが、invariant の所在が caller 側に分散している。
- Why it matters: 将来の caller が `rv != 0` を stream-local failure と誤分類して dead 化を省くと、partial write 済み connection の再利用 → wire corruption に直結する。関数内 guard が部分的であることはこのリスクを隠す。
- Recommended fix: `send_pending_h2_frames_with_deadline` 内で `rv != 0` のとき無条件に `mark_connection_dead(connection, rv)` する (write を一度も行わない失敗 — 例: nghttp2 internal NOMEM — も保守的に dead 化してよい。gRPC core も write path 失敗は transport failure として扱う)。少なくともコメントを「buffered bytes のケースのみ関数内で保証し、それ以外は caller 責務」と正確化する。
- Fix summary: `send_pending_h2_frames_with_deadline` の dead 化 guard を `rv != 0 && write_buffer_len > 0` から `rv != 0` 無条件に変更し、コメントを direct write の partial frame ケースも含む内容に更新。write 失敗時の "must never be reused" invariant が関数内で完結。
- Fix commit: `1b7ffa9`
- Verification: PHPT 15/15 pass、C unit tests pass、static analysis pass。再レビューで確認: large-frame bypass / 非 TLS direct write の partial write 失敗も `rv != 0` で漏れなく dead 化される。`mark_connection_dead` は再入安全 (dead flag set + 最初の error detail を保持) で、write を行わない失敗 (例: nghttp2 NOMEM) の保守的 dead 化も副作用なし — connection は次回 `get_persistent_connection` の `connection_usable` check で除去されるのみ。`rv == 0` 経路の挙動は不変。
- (旧 Verification) `write_buffer_len` の staleness は無いことを確認済み — 関数冒頭 (src/transport.c:1756) と末尾 (1787) で 0 にリセットされ、`write_coalescing` が true になるのはこの関数の実行中のみのため、前回 send の残骸が guard を誤発火させる経路はない。dead 化条件自体 (buffered bytes 破棄 = session/wire mismatch) は正しい。
- Notes: 既存 caller がすべて dead 化しているため現時点で観測可能なバグではない。

### REVIEW-20260610-003: request metadata 個数上限の境界テストが無い

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: commit `c693410` で `GRPC_LITE_MAX_REQUEST_METADATA_VALUES` (256) ちょうどの custom metadata values が deadline (grpc-timeout) + user-agent と共存できるようになったが、この境界 (256 values 受理 / 257 values で "exceeds maximum count" 例外) を固定するテストが tests/unit, tests/phpt に見当たらない。
- Evidence: `grep -rn "maximum count\|MAX_REQUEST_METADATA" tests/` がヒットなし。off-by-one は旧 cap 263 で「deadline 設定時に 256 個目の custom value が誤って reject される」という静かな退行だったため、テストが無いと再発検知できない。
- Expected model: guide §11 — metadata limit の境界は happy path とは別に固定する。
- Why it matters: 修正自体は正しい (検証済み、下記) が、cap の算術は今後 fixed header 追加 (例: `grpc-encoding` 対応) のたびに `GRPC_LITE_REQUEST_FIXED_HEADER_COUNT` の更新を要し、テストが無いと再び silent off-by-one になる。
- Recommended fix: 「deadline + custom user-agent + 256 custom values が全件送信される」「257 個目で例外」の unit test (test_protocol_core.c 相当) を追加する。
- Fix summary: tests/phpt/020-request-metadata-control.phpt に「`GRPC_LITE_MAX_REQUEST_METADATA_VALUES` (256) ちょうどの custom values + 全 fixed header で STATUS_OK」の境界 assert を追加 (同ファイル既存の 257 values 拒否テスト — line 146 — と対で off-by-one 再発を検知)。
- Fix commit: `3fe4c52`
- Verification: PHPT 15/15 pass (追加 assert 含む)、C unit tests pass、static analysis pass。再レビューで確認: 256 受理 (新規, line 100-107) と 257 拒否 (既存, line 146) の両境界が固定された。推奨は C unit test だったが、実 wire 経路 (deadline + user-agent 含む fixed headers と共存) を end-to-end で固定する PHPT は同等以上のカバレッジであり妥当。
- (旧 Verification) コードレビューでの検証 — fixed headers は unary_call.c:98-111 / server_streaming_call.c:107-129 ともに `:method`, `:scheme`, `:authority`, `:path`, `content-type`, `te` (6) + `grpc-timeout` (最大 1) + `user-agent` (常に 1) = 最大 8 で定数 8 と一致。cap 264 で 256 個目の custom value 追加時 `len = 263 < 264` が通り `len = 264` の exact fit、257 個目は `custom_value_count >= 256` (src/transport.c:2395) で例外。`append_grpc_timeout_request_header` の `value_strings` slot 消費 (value_count 最大 1 + 256 = 257 ≤ 264) も成立。
- Notes: 指摘はテスト欠如のみ。定数値と修正内容は正しい。

## Review Result

- Blocker: none
- High: none
- Medium: none (1 件 → `bfcf5a6` で Fixed)
- Low: none (2 件 → `1b7ffa9` / `3fe4c52` で Fixed)
- Design Decision: none
- Open findings: none — 全 3 件 Fixed を再レビューで確認済み (2026-06-10)。修正 3 commits (`bfcf5a6`, `1b7ffa9`, `3fe4c52`) による新規問題なし。検証: PHPT 15/15 pass、C unit tests pass、static analysis pass。

### 検証済みで問題なしと判断した点

- commit `5b9e499` (trace): outbound HEADERS の HPACK block hex dump 削除は妥当。HEADERS は `header_block_len` のみ記録し、`payload_hex` の対象は SETTINGS/WINDOW_UPDATE/PING/GOAWAY に限定 (src/transport.c:599-609)。DATA payload や inbound 経路から HPACK raw bytes が漏れる箇所が無いことを確認。`grpc_lite_trace_request_headers` の hash 記録という既存方針と整合。
- commit `2d331d5` (direct decode): 削除された compressed-skip 処理は到達不能であることを旧コードで確認 — `response_current_compressed` が true になり得るのは flag != 0 のときのみだが、flag == 1 は `compressed_response_seen` 分岐、flag > 1 は `malformed_response_frame` 分岐がいずれも当該 field を false に戻して `continue` するため、alloc/skip 部に true で到達する経路は存在しない。reachable な入力に対する gRPC message framing semantics (flag > 1 → malformed + RST_STREAM CANCEL、flag == 1 → `compressed_response_seen` + RST_STREAM CANCEL、empty message の即時完了、message too large → RESOURCE_EXHAUSTED 経路、read-ahead limit) は不変。`response_current_compressed` の参照が src/ 全体から消えていることを grep で確認。bench.c の変更は per-iteration の field 再初期化 1 行削除のみで、`PHP_GRPC_LITE_ENABLE_BENCH` 配下の diagnostic 境界を越えない。
- commit `b7e17fd` の安全性: `release_persistent_connection_entry_if_mismatched` が他所から参照中の connection を破棄する経路、および double-free 経路が無いことを確認 (REVIEW-20260610-001 の Verification 参照)。`discard_persistent_connection(key, len, NULL)` 相当の経路でも、cache された connection を leak ではなく detached 規約 (`detached_from_cache` + unowned 時破棄) で解放するのは discard の意味論として正しい。
