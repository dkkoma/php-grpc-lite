# h2c write coalescing 有効化 ドメインモデルレビュー 2026-06-12

## Scope

- `src/transport.c`(commit `99f7912`: `send_pending_h2_frames_with_deadline` の `connection->write_coalescing = connection->tls;` → `= true;`)
- 参照: `src/transport_core.c` / `src/transport_core.h`(coalesce capacity 導出)、`src/transport.h`(`write_buffer*` state)、`src/diagnostic/bench.c`(別系統の send_callback)

## Reviewer Role

- `HTTP/2 / gRPC domain model reviewer`(docs/verification/protocol-model-review-guide.md 準拠)

## Review Prompt Summary

- 平文 (h2c) 接続でも write coalesce buffer を有効にする 1 行変更について、(1) 平文 write path の partial write / deadline / EAGAIN 処理が TLS と共有済みか・buffer path に TLS 前提が残っていないか、(2) flush 失敗時の mark_connection_dead の失敗セマンティクスが平文 coalesced write でも同一に成立するか、(3) 関数呼び出しを跨いで stale な buffered bytes が残らないか(preflight drain の call == NULL 経路含む)、(4) メモリ増のモデル、(5) coalescing による wire 上のバイト遅延が preface/SETTINGS 処理と相互作用しないか、を検証。

## 検証結果(指摘なしの確認事項)

ガイド §3 のドメインモデルで言えば、`write_coalescing` / `write_buffer*` / `current_write_deadline_abs_us` はすべて HTTP/2 Connection scope の state であり、今回の変更は Connection 内の write 戦略フラグの条件変更のみ。gRPC Call / Channel / Stream の責務境界には触れていない。

1. **平文 write path の共有(確認済み、問題なし)**: coalesce buffer の flush は `h2_connection_flush_write_buffer` → `h2_connection_write_all` → `h2_connection_send`(transport.c:1228〜)で、`connection->ssl == NULL` 分岐は `send(2)` + `EAGAIN/EWOULDBLOCK` → `poll_fd_until_deadline(POLLOUT)` → `remaining_timeout_us_for_deadline` の deadline-aware retry を持つ(1289〜1325)。partial write は `h2_connection_write_all`(1328)の `total_written` ループで吸収。error detail は平文分岐が `send failed: %s` / `send poll failed: %s` / deadline 文言を自前で設定し、`last_ssl_error` は平文接続では一度も書かれず 0 のままなので、失敗時に `call->last_ssl_error` へコピーされても(1776)誤情報にならない。buffer path(`h2_connection_buffer_or_write`、1353〜1385)自体は memcpy と容量判定のみで TLS 前提なし。ENOMEM 時の error detail も transport 語彙(`failed to allocate HTTP/2 write buffer`)。
2. **失敗セマンティクス(確認済み、コメントも更新済み)**: `send_callback`(1711)は `h2_connection_buffer_or_write` が SUCCESS を返した時点で `length` を返すため、buffered bytes は nghttp2 session 上「送信済み」として account される。flush 失敗時はそのバイトが wire に乗らないまま破棄されるので session state と wire が乖離する。1764〜1769 のコメントが direct write の partial frame と coalesced bytes の discard の両方を明記しており、`mark_connection_dead`(742)で `dead = true` → `connection_usable` が false → persistent cache から reuse されない。平文・TLS で完全に同一ロジック。
3. **stale buffered bytes なし(確認済み)**: production code で `nghttp2_session_send` を呼ぶのは `send_pending_h2_frames_with_deadline`(1752)のみ(bench.c は専用 session + `bench_send_callback` で `write_coalescing` 経路を通らない)。同関数は entry(1751)と exit(1783)で `write_buffer_len = 0`、exit で `write_coalescing = false` にリセットする。途中失敗(nghttp2 エラー、flush 部分失敗)はすべて `rv != 0` → `mark_connection_dead` に合流し、buffered bytes が次回呼び出しへ持ち越されて re-order / duplicate される経路はない。preflight drain(`drain_pending_connection_data_for_reuse`、829〜)の `send_pending_h2_frames_with_deadline(connection, NULL, deadline_abs_us)`(895)も同一関数なので同じ flush 保証を受け、失敗時は dead 化して reuse を拒否する。`connection_send`(1210、coalesce を迂回する direct send)の呼び出し元は diagnostic bench のみで、unary / server streaming の送信はすべて `send_pending_h2_frames*` 経由。
4. **メモリモデル(確認済み)**: `write_buffer` は初回 coalesced write 時に lazy 確保(1368〜1376)、容量は `h2_write_coalesce_capacity_for_max_frame_size`(transport_core.c:68、デフォルト 4×(16,384+9) = 65,572B、上限 1MB)。persistent cache は `GRPC_LITE_MAX_PERSISTENT_CONNECTIONS = 128`(transport_core.h:22)で bound されるため、デフォルト構成の worst case は約 8.4MB/cache。非 persistent 接続の分は `destroy_h2_connection`(181〜182)で解放される。
5. **latency / SETTINGS flush(確認済み、問題なし)**: coalescing によるバイト遅延は単一の `nghttp2_session_send` ループ内に閉じ、関数 return 前に必ず flush される。nghttp2 は session_send 中に recv しないので、「buffer に SETTINGS を抱えたまま server 応答を待つ」経路は存在しない。connection setup(create_h2_connection:1541)では client connection preface + SETTINGS(+ connection-level WINDOW_UPDATE)が 1 回の send にまとまるが、これは gRPC Core 等と同様の挙動で、SETTINGS ACK の受信は setup 後の read path(deadline 付き)で行われるため、ループ完了前に SETTINGS が wire に乗っていることへの依存はない。preflight drain 中の WINDOW_UPDATE / PING ACK も pass ごとに flush される。

## Issues

### REVIEW-20260612-001: `write_coalescing` フラグが production 経路で常に true となり、フラグとしての意味が縮退

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: 今回の変更で `send_pending_h2_frames_with_deadline` 内の write pass は無条件に coalescing になり、`h2_connection_buffer_or_write` の `if (!connection->write_coalescing)` 分岐(transport.c:1359)は production では到達しない。フラグの実質的な意味は「TLS 接続の最適化 ON/OFF」から「いま send_pending の write pass 内にいる(= 関数末尾の flush が保証されている)」へ変わった。
- Evidence: `src/transport.c:1750`(常時 true)、`src/transport.c:1782`(exit で false)、production の `send_callback` 呼び出し元が 1752 の `nghttp2_session_send` のみであること。
- Expected model: HTTP/2 Connection の state は意味が一意であるべき。フラグを残すなら「flush 保証のある write pass の内側」という invariant をコメントで固定するか、フラグ自体を除去して send_callback 側で常に buffer する設計に畳む。
- Why it matters: 将来 `send_callback` を別経路(例: setup や診断)から呼ぶ変更が入ったとき、「coalescing = flush 保証付き pass 内」という暗黙の前提を知らないと unflushed bytes バグを作り得る。現時点では bench.c が独立 callback のため実害なし。
- Recommended fix: フォローアップで (a) `write_coalescing` を除去し send_pending 内で完結させる、または (b) transport.h の `write_coalescing` 宣言に「set/cleared only by send_pending_h2_frames_with_deadline; implies end-of-pass flush」と invariant を明記する。
- Fix summary: 案 (b) を採用。`transport.h` の `write_coalescing` 宣言に「send_pending_h2_frames_with_deadline 内のみで true(末尾 flush 保証付き)、他経路は false のまま」と invariant を明記。
- Fix commit: perf/h2c-write-coalescing(レビュー反映コミット)
- Verification: C unit(coalesce 境界テストは #4 で追加済み)、PHPT 15/15・PHPUnit 30/30 PASS。
- Notes: 1751 の entry-side `write_buffer_len = 0` は、dead 化済み接続に万一バイトが残っていても「送らずに捨てる」方向の防御で、duplicate/re-order 側には倒れない。妥当。

### REVIEW-20260612-002: 平文での frame trace タイミングが wire write より先行する(diagnostic のみ)

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `grpc_lite_trace_outbound_frame`(transport.c:1731)は buffer 受理時点で frame を記録するため、平文でも frame trace 行が対応する `wire.socket_write` 行より前に出るようになる。flush 失敗時は「trace 上は emit されたが wire には乗らなかった frame」が記録され得る。
- Evidence: `src/transport.c:1728-1731`(buffer_or_write 成功直後に trace)、`src/transport.c:1298`(`wire.socket_write` は実 send 時)。
- Expected model: trace の語彙どおり「outbound frame = nghttp2 が emit した frame」「wire.* = socket I/O」と読めば矛盾はないが、平文 trace を消費する既存スクリプト(trace-io-probe.sh 等)が「frame 行 ≒ wire 行と 1:1 で隣接」を仮定していると解析がずれる。
- Why it matters: TLS では既に同じ挙動なので新規モデル破壊ではない。診断系の互換性メモとして残す。
- Recommended fix: trace ドキュメントまたは trace-io-probe 側に「outbound_frame は emit 時、wire.* は syscall 時」の注記を 1 行追加。コード変更不要。
- Fix summary: `docs/guides/code-reading-guide.md` の trace 説明に「wire.frame_out は buffer 投入時、wire.* は syscall 時」の注記を追加。trace-io-probe.sh は `wire.socket_write` / `wire.tls_write` の行数カウントのみで隣接性に依存しないことを確認済み。
- Fix commit: perf/h2c-write-coalescing(レビュー反映コミット)
- Verification: ドキュメントのみ。
- Notes: issue doc の trace 計測(692 → 190)は `wire.socket_write` 行のカウントなので本件の影響を受けていない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `2`
- Design Decision: `none`
