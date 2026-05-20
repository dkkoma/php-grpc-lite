# persistent preflight drain HTTP/2 domain model review 2026-05-20

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `docs/issues/open/2026-05-20-persistent-preflight-drain.md`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer

## Review Prompt Summary

- persistent connection reuse直前のpending TLS/socket data drainについて、connection/stream lifecycle、nghttp2 parser state、error taxonomy、production behaviorを確認した。

## Issues

### REVIEW-20260520-001: drain上限到達をsafe reuseとして扱っている

- Severity: High
- Status: Fixed
- Reviewer role: HTTP/2/gRPC domain model reviewer
- Finding: `drain_pending_connection_data_for_reuse()` は `GRPC_LITE_PREFLIGHT_DRAIN_MAX_ITERATIONS` または `GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES` に到達してloopを抜けた場合も、`connection_usable(connection)` がtrueならそのまま再利用を許可している。
- Evidence: `ext/grpc/transport.c:842` のbounded loopと `ext/grpc/transport.c:895` のreturn。`docs/issues/open/2026-05-20-persistent-preflight-drain.md` のscopeは「pending dataを短くdrainし、drain後にusableなら再利用」だが、上限到達時は「pending dataなし」ではなく「connection state未確定」。
- Expected model: preflightはHTTP/2 Connectionを次のRPCへ載せてよい状態へ遷移させるgateであり、pending bytesをEAGAIN/WANT_READまでparserへ渡し切った、またはGOAWAY/EOF/protocol errorでdraining/deadへ遷移した、のどちらかを明示すべき。bounded drainの上限到達は別状態として扱い、safe reuseと同一視しない。
- Why it matters: 上限の先にGOAWAY、protocol error、または大量のconnection-level/control dataが残っている場合でも新規streamをsubmitでき、HTTP/2 Connection lifecycle上はdraining/deadにすべきconnectionへ新しいgRPC Callを載せる可能性がある。nghttp2 parser state自体は継続可能でも、preflight gateの意味が「安全確認」から「一部だけ読んだ」に変わる。
- Recommended fix: loopがEAGAIN/WANT_READ/WANT_WRITEではなく上限到達で終わったことを検出し、connectionをdrainingまたはdeadとしてcacheから外す。少なくとも明示的な状態名とtrace/error detailを持たせ、上限到達時にreuseしない。
- Fix summary: `drain_pending_connection_data_for_reuse()` に `reached_read_boundary` を追加し、EAGAIN/WANT_READ/WANT_WRITEへ到達しないままbounded loopを抜けた場合は `persistent preflight drain limit exceeded` を記録して `mark_connection_draining(connection, 0, NGHTTP2_NO_ERROR)` でreuse不可にするよう修正された。
- Fix commit: this commit
- Verification: Re-review static check: `ext/grpc/transport.c:844`-`ext/grpc/transport.c:916` で上限到達時にsafe reuseへ進まずfalseを返すことを確認。
- Notes: 上限値そのものではなく、上限到達時のdomain stateが未定義な点を指摘していた。2026-05-20再レビューで解消確認。

### REVIEW-20260520-002: preflight drain後のconnection-level outbound frameがflushされない

- Severity: Medium
- Status: Fixed
- Reviewer role: HTTP/2/gRPC domain model reviewer
- Finding: pending dataを `nghttp2_session_mem_recv()` に渡した後、`PING ACK`、`SETTINGS ACK`、`WINDOW_UPDATE` 等のconnection-level outbound frameが生成されても、preflight内では `send_pending_h2_frames(connection, NULL)` でflushされない。
- Evidence: `ext/grpc/transport.c:884`-`ext/grpc/transport.c:892` はmem_recv後に `nghttp2_session_want_write()` を確認しない。通常のunary/server streaming recv loopはmem_recv後にwant_writeならflushする（`ext/grpc/unary_call.c:179`、`ext/grpc/server_streaming_call.c:314`）。issue scopeにも「ACK等で発生したpending outbound frameは `send_pending_h2_frames(connection, NULL)` でflushする」とある。
- Expected model: preflight drainはHTTP/2 Connectionのcontrol frame lifecycleを完結させる処理であり、次のgRPC Call作成に依存せず、受信により発生したconnection-level writeをconnection scopeで処理すべき。
- Why it matters: 次のrequest送信時に結果的にflushされるケースは多いが、request準備中のvalidation failureや、PING/SETTINGS ACKをRPC stream lifecycleへ便乗させる構造になる。connection-level control responseがcall orchestrationへ漏れ、production behaviorとしてpreflightの責務が不完全になる。
- Recommended fix: mem_recvでbytesを処理した後、`nghttp2_session_want_write(connection->session)` がtrueなら `send_pending_h2_frames(connection, NULL)` を呼び、失敗時はconnection failureとしてdeadにする。preflight固有のdeadline/timeout方針もconnection setup/reuse scopeとして明示する。
- Fix summary: preflight drain中に `nghttp2_session_want_write(connection->session)` を確認し、trueならcurrent RPC deadlineを渡した `send_pending_h2_frames_with_deadline(connection, NULL, deadline_abs_us)` でconnection-level outbound frameをflushするよう修正された。flush failureはconnection failureとしてdeadにする。
- Fix commit: pending
- Verification: Re-review static check: `ext/grpc/transport.c:898`-`ext/grpc/transport.c:905` と `ext/grpc/transport.c:1800`-`ext/grpc/transport.c:1837` でpreflight scopeのflushとdeadline伝搬を確認。
- Notes: request送信で後続flushされることを期待する構造ではなくなった。2026-05-20再レビューで解消確認。

### REVIEW-20260520-003: nghttp2 parser errorをsocket resetへ分類している

- Severity: Low
- Status: Fixed
- Reviewer role: HTTP/2/gRPC domain model reviewer
- Finding: preflight drain中の `nghttp2_session_mem_recv()` failureを `ECONNRESET` として `mark_connection_dead()` に渡しており、HTTP/2 parser/connection errorとsocket/TCP resetを同じtaxonomyへ畳んでいる。
- Evidence: `ext/grpc/transport.c:885`-`ext/grpc/transport.c:888`。通常recv loopは `mark_connection_dead(connection, rv)` としてnghttp2 error codeを保持する（`ext/grpc/unary_call.c:170`-`ext/grpc/unary_call.c:171`、`ext/grpc/server_streaming_call.c:309`-`ext/grpc/server_streaming_call.c:310`）。
- Expected model: nghttp2 parser failureはHTTP/2 Connection failureであり、socket/TLS I/O failureとは区別して `last_error` / detailに残すべき。error taxonomyは TCP EOF/TLS/socket error/nghttp2 connection error/GOAWAY を分ける。
- Why it matters: diagnosticsとstatus resolutionで「peer reset」なのか「HTTP/2 parser/protocol failure」なのかが失われ、persistent cache evictionの理由やproduction incident調査が誤読される。
- Recommended fix: `nghttp2_session_mem_recv()` の戻り値を保存し、`mark_connection_dead(connection, rv)` を使う。必要ならdetailに `nghttp2_strerror(rv)` とpreflight contextを含める。
- Fix summary: `nghttp2_session_mem_recv()` のnegative returnを `rv` として保持し、`nghttp2_strerror((int) rv)` を含むpreflight detailを記録したうえで `mark_connection_dead(connection, (int) rv)` に渡すよう修正された。
- Fix commit: pending
- Verification: Re-review static check: `ext/grpc/transport.c:889`-`ext/grpc/transport.c:893` でHTTP/2 parser error codeがsocket resetへ変換されないことを確認。
- Notes: connectionをdeadにする判断自体は妥当で、分類の問題だった。2026-05-20再レビューで解消確認。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none

## Re-review 2026-05-20

- Scope: previous findings in this file plus current persistent preflight drain lifecycle changes only.
- Result: previous High/Medium/Low findings are fixed.
- New HTTP/2/gRPC lifecycle findings: none.
- Verification: reviewer static check only; no production files edited.
