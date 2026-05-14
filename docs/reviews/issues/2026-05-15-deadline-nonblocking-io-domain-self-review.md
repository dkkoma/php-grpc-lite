# deadline nonblocking I/O domain model self review 2026-05-15

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/unary_call.c`
- `ext/grpc/server_streaming_call.c`
- `ext/grpc/internal.h`
- `docs/issues/open/2026-05-15-native-deadline-socket-timeout-hotpath.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- Review whether replacing per-I/O socket timeout mutation with nonblocking socket I/O and deadline-aware poll preserves HTTP/2 connection, gRPC call deadline, TLS, and error taxonomy responsibilities.

## Issues

### REVIEW-20260515-001: nonblocking socket mode is a transport-level state, not call state

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 / gRPC domain model reviewer`
- Finding: TLS success path and h2c setup now leave the connection fd in nonblocking mode for the lifetime of the HTTP/2 connection.
- Evidence: `ext/grpc/transport.c:configure_tls_connection`, `ext/grpc/transport.c:create_h2_connection`, `ext/grpc/transport.c:h2_connection_send`, `ext/grpc/transport.c:connection_recv`
- Expected model: Socket blocking mode belongs to HTTP/2 Connection transport state. gRPC Call owns deadline semantics, and transport applies the absolute deadline during I/O waits.
- Why it matters: If nonblocking mode were treated as per-call state, persistent connection reuse would require repeated mode mutation and would reintroduce kernel-boundary overhead. Keeping it as connection state is consistent with nghttp2-driven transport ownership.
- Recommended fix: Keep nonblocking mode as the connection default and ensure every send/recv path handles `EAGAIN` / `EWOULDBLOCK` / SSL WANT states through `poll_fd_until_deadline()`.
- Fix summary: Current implementation follows this model. No code change required.
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh` passed 15/15; `./tools/test/check-c-static-analysis.sh` passed; strace shows per-RPC `SO_RCVTIMEO` / `SO_SNDTIMEO` mutation removed.
- Notes: This is an explicit design decision because it changes the transport invariant from mostly-blocking fd with per-call socket timeout to nonblocking fd with explicit wait points.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1`
