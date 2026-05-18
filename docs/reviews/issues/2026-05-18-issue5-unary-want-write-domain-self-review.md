# issue #5 unary want_write gate domain review 2026-05-18

## Scope

- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/transport.c`
- `docs/issues/open/2026-05-18-github-issue-5-tls-headers-data-write-attribution.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer (parent-agent self review; subagent spawn failed due agent thread limit)

## Review Prompt Summary

- Review whether gating unary receive-loop `send_pending_h2_frames()` with `nghttp2_session_want_write()` correctly models HTTP/2 control-frame lifecycle, call/connection ownership, error handling, and production/diagnostic boundaries.

## Issues

### REVIEW-20260518-001: none

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: `nghttp2_session_mem_recv()` may enqueue outbound control frames such as PING ACK, WINDOW_UPDATE, RST_STREAM, or GOAWAY response handling. Calling `send_pending_h2_frames()` only when `nghttp2_session_want_write()` is true preserves that lifecycle while avoiding empty send cycles.`
- Evidence: `ext/grpc/unary_call.c`, `ext/grpc/server_streaming_call.c`
- Expected model: `A receive loop should flush pending HTTP/2 outbound frames after processing inbound bytes only when nghttp2 reports pending write work. It should not call the transport send path when there is no HTTP/2 write state.`
- Why it matters: `This keeps unary and server streaming control-frame semantics consistent without delaying required ACK/control frames.`
- Recommended fix: `Keep the unary receive-loop gate aligned with server streaming.`
- Fix summary: `Unary receive loop now checks nghttp2_session_want_write(connection->session) before calling send_pending_h2_frames() after nghttp2_session_mem_recv().`
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh && ./tools/test/check-c-static-analysis.sh`
- Notes: `This does not coalesce or delay PING ACK. It only removes empty session_send calls when nghttp2 has no pending outbound frames.`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 accepted`
