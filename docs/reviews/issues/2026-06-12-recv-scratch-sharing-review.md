# recv scratch buffer 共有化 (perf/unary-recv-buffer-size) HTTP/2/gRPC ドメインモデルレビュー 2026-06-12

## Scope

- `src/transport.c` (`h2_connection_recv_scratch()`, `destroy_h2_connection()`, `drain_pending_connection_data_for_reuse()`)
- `src/transport.h` (`h2_connection.recv_scratch` / `recv_scratch_len`, `server_streaming_call_state.recv_buf` 削除)
- `src/unary_call.c` (unary receive loop)
- `src/server_streaming_call.c` (server streaming `next()` receive loop)
- 対象 diff: `git diff main...perf/unary-recv-buffer-size` (commit a1f5c79)

## Reviewer Role

- `HTTP/2 / gRPC domain model reviewer` (docs/verification/protocol-model-review-guide.md 準拠)

## Review Prompt Summary

- 3 つの receive buffer (unary 16KB stack / server streaming per-call emalloc(65536) / preflight drain 4KB stack) を h2_connection 共有の lazily-allocated 64KB scratch に統合する変更について、(1) buffer 共有の安全性 (受信ループの同時実行・再入)、(2) scratch の lifetime (destroy 後使用・leak)、(3) pemalloc/persistent 整合、(4) `nghttp2_session_mem_recv` のデータ保持仮定、(5) drain path の read 粒度変更 (4KB→64KB) の意味的差異、を確認する。

## 検証結果 (指摘に至らなかった確認事項)

1. **Buffer 共有安全性**: unary loop (`unary_call.c:146-186`)、server streaming `next()` loop (`server_streaming_call.c:265-320`)、preflight drain (`transport.c:857-921`) はいずれも同一 PHP thread 上で同期的に完走する。`nghttp2_session_mem_recv` の callback (`on_header_callback` / `on_data_chunk_recv_callback` / `on_stream_close_callback` / send callback) は `connection_recv` も PHP userland も呼ばないため、scratch 使用中に別の receive loop が再入する経路はない。drain は connection 取得 (reuse preflight, `transport.c:941,974`) 時のみで、call の receive loop とは時間的に排他。ZTS では persistent connection cache が per-thread module globals (`module.h` `ZEND_BEGIN_MODULE_GLOBALS(grpc_lite)`) のため、connection (= scratch) が thread 間で共有されることもない。
2. **Lifetime**: connection は `pecalloc` (`transport.c:1493`) で zero-init され `recv_scratch == NULL` から開始。解放は `destroy_h2_connection()` (`transport.c:198-200`) の `pefree(connection->recv_scratch, connection->persistent)` のみで、connection 解放経路 (`destroy_detached_connection_if_unowned` / `destroy_persistent_connection_entry` / setup 失敗各 path) はすべて `destroy_h2_connection()` に集約されており leak path はない。`destroy_detached_connection_if_unowned()` は各 receive loop 終了後にのみ呼ばれ、scratch 使用中に connection が破棄される経路はない (`mark_connection_dead` / `detach_persistent_connection_by_ptr` は free しない)。
3. **Allocation 整合**: `connection->persistent` は `pecalloc` 直後 (`transport.c:1494`) に一度だけ設定され以後不変のため、`pemalloc(…, persistent)` と `pefree(…, persistent)` の対は崩れない。非 persistent connection の scratch は emalloc 相当で、connection 自体が request 内で破棄される (最悪でも ZendMM が request shutdown で回収)。`pemalloc` は OOM 時に fatal (`zend_out_of_memory`) であり NULL を返さないため、NULL チェック省略は実害なし (`write_buffer` 側 `transport.c:1387` の NULL チェックは元々 dead code)。
4. **mem_recv のデータ保持**: direct-decode path は `memcpy(ZSTR_VAL(call->response_payload)+…, data+offset, take)` (`transport.c:2681`) で同期コピー、通常 path は `smart_str_appendl(&call->body, …)` (`transport.c:1907`)、header は `zend_string_init` でコピー。scratch へのポインタが `nghttp2_session_mem_recv` の return 後に保持される箇所はない。
5. **Flow-control との混同なし**: scratch size は read 粒度のみで、stream/connection receive window (`SETTINGS_INITIAL_WINDOW_SIZE` / connection `WINDOW_UPDATE`) の設定とは独立。domain scope 的にも receive scratch は HTTP/2 Connection (transport I/O) の state であり、`server_streaming_call_state` (Server Streaming Resource) から connection へ移したことはむしろ責務配置の改善。

## Issues

### REVIEW-20260612-001: 単一 receive loop 不変条件が comment のみで担保されている

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `h2_connection_recv_scratch()` の安全性は「connection あたり同時に 1 つの receive loop しか走らない」という不変条件に依存するが、これは comment (`transport.c:169-172`) で説明されているだけで、コード上の guard がない。将来 callback 内からの read や bidi streaming 追加などで再入経路ができた場合、scratch の上書き破壊が silent に起きる。
- Evidence: `src/transport.c:167-178` (`h2_connection_recv_scratch`)、`src/unary_call.c:167-169` / `src/server_streaming_call.c:304-306` (`current_read_call` の set/clear)。なお drain path は `current_read_call` を設定しないため、不変条件の「単一 current_read_call」という comment 上の根拠は drain には形式的に当てはまらない (drain 中の read-ahead queue 制限 bypass は drain cap 64KB で有界、かつ本変更以前からの挙動)。
- Expected model: HTTP/2 Connection の transport scratch は「同時に高々 1 つの receive 経路」という connection-level invariant を構造的に表現する。
- Why it matters: invariant が暗黙だと、将来の orchestration 変更 (streaming read-ahead 拡張等) で buffer 共有前提が壊れても検出されない。
- Recommended fix: `h2_connection_recv_scratch()` 先頭に `ZEND_ASSERT(connection->current_read_call == NULL);` を追加する (各 loop は scratch 取得後に `current_read_call` を設定するため debug build で再入を検出できる)。あわせて comment に drain path も同 invariant に含まれる旨を一行追記。
- Fix summary: `h2_connection_recv_scratch()` 先頭に `ZEND_ASSERT(connection->current_read_call == NULL);` を追加し、comment に drain path も invariant に含まれる旨を追記
- Fix commit: perf/unary-recv-buffer-size ブランチ(レビュー反映コミット)
- Verification: PHPT 15/15 PASS(再ビルド後)
- Notes: 現実装に bug はない。防御的 guard の提案のみ。

### REVIEW-20260612-002: persistent connection が 64KB scratch を lifetime 全体で保持する

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: scratch は lazily allocated だが解放は `destroy_h2_connection()` のみのため、persistent connection は request をまたいで 64KB (write_buffer と合わせ最大 128KB) を connection ごとに保持し続ける。cache 上限 `GRPC_LITE_MAX_PERSISTENT_CONNECTIONS` × 64KB が常駐上限。
- Evidence: `src/transport.c:167-178`, `transport.c:198-200`
- Expected model: per-call allocation の削減 (perf issue docs/reviews/issues/open/2026-06-12-unary-recv-buffer-size.md) と memory residency のトレードオフ。
- Why it matters: FrankenPHP worker mode のような長寿命 process では常駐メモリとして残るが、上限が cache サイズで有界なので問題にならない規模。
- Recommended fix: なし (現状維持)。再検討条件: persistent cache 上限を大きく引き上げる、または scratch size を可変にする場合。
- Fix summary: `n/a`
- Fix commit: `n/a`
- Verification: コードレビューのみ
- Notes: drain の read 粒度 4KB→64KB についても確認した。`GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES=65536` / `MAX_ITERATIONS=64` は不変で、reuse 判定の分類 (read boundary 到達→reusable / cap 超過→draining / error→dead) は変わらない。差異は cap 判定が read 後のため、draining 判定前に処理されうるバイト数の上振れが従来の最大 約68KB から 約128KB になる点のみで、いずれも当該 connection は reuse されず draining として破棄されるため意味的影響なし (TLS 側は SSL_read が 1 record 単位のため実質変化はさらに小さい)。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `1`
