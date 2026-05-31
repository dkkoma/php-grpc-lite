# persistent preflight drain C extension safety review 2026-05-20

## Scope

- `ext/grpc/transport.c`
- `docs/issues/open/2026-05-20-persistent-preflight-drain.md`
- `docs/verification/protocol-model-review-guide.md`
- `docs/design/http2-transport-design.md`
- `docs/guides/code-reading-guide.md`

## Reviewer Role

- PHP extension / C safety reviewer

## Review Prompt Summary

- Review current `main` branch changes around persistent connection preflight drain, focusing on stack buffer safety, `SSL_read()` / `recv()` nonblocking behavior, `nghttp2_session_mem_recv()` error handling, loop bounds, persistent resource lifetime, and diagnostic/docs accuracy.

## Issues

### REVIEW-20260520-001: preflight drainで生成されたpending outbound frameをflushしていない

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PHP extension / C safety reviewer`
- Finding: `drain_pending_connection_data_for_reuse()` は受信bytesを `nghttp2_session_mem_recv()` に渡すが、その結果queueされる `SETTINGS ACK` / `PING ACK` / `WINDOW_UPDATE` / `RST_STREAM` などを `send_pending_h2_frames()` でflushしないまま `connection_usable()` なら再利用可として返す。
- Evidence: `ext/grpc/transport.c:884` で `nghttp2_session_mem_recv()` を呼び、`ext/grpc/transport.c:890` 以降は `nghttp2_session_want_write()` / `send_pending_h2_frames()` を確認せずにloop継続または成功する。実装issue側は `docs/issues/open/2026-05-20-persistent-preflight-drain.md:27` でACK等のflushをscopeに入れている。プロトコルレビュー基準も `docs/verification/protocol-model-review-guide.md:154` でpending control frame / ACK / WINDOW_UPDATEの必要時flushを要求している。
- Expected model: preflight drainはidle connectionを「次RPCに渡してよいHTTP/2 connection state」へ戻す責務を持つため、inbound control frame処理で発生したoutbound control frameも同じpreflight lifecycle内でflushし、flush失敗はconnection failureとして扱う。
- Why it matters: ACKやflow-control更新を未flushのまま新規RPCを開始すると、wire stateがpreflight完了済みという診断・設計とずれる。次RPCのrequest送信時に偶然flushされる場合もあるが、preflightの責務が「inboundだけ処理してoutboundは次RPCへ持ち越す」状態になり、send failureやprotocol pressureの原因を新規RPCに誤帰属しやすい。
- Recommended fix: `preflight_persistent_connection()` / `drain_pending_connection_data_for_reuse()` に現在RPCのdeadlineを渡し、drain後に `nghttp2_session_want_write(connection->session)` がtrueなら `send_pending_h2_frames(connection, NULL)` 相当をそのdeadlineで実行する。既存 `send_pending_h2_frames(connection, NULL)` は `setup_deadline_abs_us` を使うため、persistent reuse時には過去deadlineになり得る点も同時に直す。
- Fix summary: `drain_pending_connection_data_for_reuse()` が `nghttp2_session_mem_recv()` 後に `nghttp2_session_want_write()` を確認し、pending outbound frameを `send_pending_h2_frames_with_deadline(connection, NULL, deadline_abs_us)` でpreflight中にflushするようになった。`preflight_persistent_connection()` もreuse時のdeadlineを受け取り、persistent reuseで古い `setup_deadline_abs_us` に依存しない経路になった。
- Fix commit: `this commit`
- Verification: `ext/grpc/transport.c:888` から `ext/grpc/transport.c:906` でACK等のflushとfailure時dead化を確認。`ext/grpc/transport.c:1644` でcurrent RPC deadlineがpreflightへ渡され、`ext/grpc/transport.c:1800` から `ext/grpc/transport.c:1847` でfallback deadline付きsend helperを確認。`./tools/test/check-c-static-analysis.sh` passed。
- Notes: `SSL_ERROR_WANT_WRITE` はread boundary扱いのままだが、このre-reviewではnghttp2がqueueしたHTTP/2 outbound frameの未flushは残っていないと判断した。

### REVIEW-20260520-002: drain上限到達時に未確認bytesを残したままreuse成功にしている

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PHP extension / C safety reviewer`
- Finding: drain loopは `GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES` または `GRPC_LITE_PREFLIGHT_DRAIN_MAX_ITERATIONS` に達すると、socket/TLSをEAGAIN/WANT_READまで読み切ったか確認せず `connection_usable(connection)` を返す。
- Evidence: `ext/grpc/transport.c:842` のloop条件が上限到達で終了し、`ext/grpc/transport.c:895` で成功扱いになる。上限到達時のerror detail設定、dead/draining化、追加peek確認がない。
- Expected model: preflightは「idle connectionに残っているcontrol dataを処理済み」または「処理し切れないためreuseしない」のどちらかを明示すべきで、bounded drainの安全弁に当たった状態はreuse可ではない。
- Why it matters: 大量のconnection-level frameや攻撃的なpeerにより64KiB分だけ処理して上限に達した場合、後続に `GOAWAY` / protocol error / close_notify が残っていても新規streamを作れる。bounded loop自体はDoS対策として妥当だが、cap到達は正常なdrain完了と区別する必要がある。
- Recommended fix: 上限到達を検出したら `last_error_detail` に「persistent preflight drain limit exceeded」を残し、connectionをdead/drainingとしてcacheから外す。reuseを許す場合でも、最後にnonblocking peek/readでEAGAIN/WANT_READまで到達したことを確認する。
- Fix summary: drain loopが `reached_read_boundary` を明示的に追跡し、byte/iteration上限に達してEAGAIN/WANT_READ/WANT_WRITE境界へ到達していない場合は `persistent preflight drain limit exceeded` を記録してconnectionをdraining化し、reuseしないようになった。
- Fix commit: `this commit`
- Verification: `ext/grpc/transport.c:844` から `ext/grpc/transport.c:859` と `ext/grpc/transport.c:877` から `ext/grpc/transport.c:880` でread boundary検出を確認。`ext/grpc/transport.c:910` から `ext/grpc/transport.c:914` で上限到達時にreuse不可へ倒すことを確認。`./tools/test/check-c-static-analysis.sh` passed。
- Notes: stack bufferサイズ4096 bytes自体はC stack safety上は妥当。

### REVIEW-20260520-003: nghttp2 preflight error detailがerror taxonomyを落としている

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PHP extension / C safety reviewer`
- Finding: preflight drain中の `nghttp2_session_mem_recv()` failureをすべて `ECONNRESET` と汎用messageへ変換しており、既存recv loopが保持しているnghttp2 negative error codeと `nghttp2_strerror()` diagnosticsを失う。
- Evidence: `ext/grpc/transport.c:885` は戻り値を直接比較して捨て、`ext/grpc/transport.c:886` で汎用detail、`ext/grpc/transport.c:887` で `mark_connection_dead(connection, ECONNRESET)` を呼ぶ。一方、通常RPC read pathは `ext/grpc/unary_call.c:168` 以降で `rv` を保持し、`mark_connection_dead(connection, rv)` に渡す。
- Expected model: nghttp2 parser/session failureはsocket errnoではなくHTTP/2 session errorとして分類し、connection failure detailにもnghttp2 errorを残す。
- Why it matters: preflightでだけprotocol/parser failureが「connection reset」に見えるため、診断やレビュー記録でTLS/socket問題とHTTP/2 protocol問題を切り分けにくくなる。
- Recommended fix: `ssize_t rv = nghttp2_session_mem_recv(...)` として保持し、`rv < 0` なら `mark_connection_dead(connection, (int) rv)` を使う。必要ならdetailに `nghttp2_strerror((int) rv)` を含める。
- Fix summary: `nghttp2_session_mem_recv()` の戻り値を `ssize_t rv` として保持し、negative errorでは `nghttp2_strerror((int) rv)` をdetailへ残して `mark_connection_dead(connection, (int) rv)` に渡すようになった。
- Fix commit: `this commit`
- Verification: `ext/grpc/transport.c:889` から `ext/grpc/transport.c:893` でnghttp2 error taxonomyを保持していることを確認。`./tools/test/check-c-static-analysis.sh` passed。
- Notes: 既存の通常read pathと合わせる変更で、production surfaceには内部error codeを直接露出しない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Re-review 2026-05-20

- Scope: previous findings in this file plus PHP extension/C safety regression check for the same persistent preflight drain diff.
- Result: previous findings are resolved; no new PHP extension/C safety findings remain in the reviewed scope.
- Verification: code review of `ext/grpc/transport.c` and `ext/grpc/internal.h`; `./tools/test/check-c-static-analysis.sh` passed.
- Note: an initial nested invocation `docker compose run --rm dev ./tools/test/check-c-static-analysis.sh` failed because the host-side wrapper calls `docker` internally; the intended host-side wrapper invocation passed.
