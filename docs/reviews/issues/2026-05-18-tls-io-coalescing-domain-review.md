# TLS I/O coalescing domain review 2026-05-18

## Scope

- `ext/grpc/transport.c`
- `ext/grpc/internal.h`
- `docs/issues/open/2026-05-18-github-issue-2-tls-write-coalescing.md`
- `docs/issues/open/2026-05-18-github-issue-3-tls-read-buffering.md`

## Reviewer Role

- `HTTP/2 / gRPC transport domain model reviewer`

## Review Prompt Summary

- TLS write coalescing and OpenSSL read-ahead changes were reviewed for nghttp2 send callback semantics, flush/error/deadline boundaries, persistent connection lifecycle, TLS/h2c behavior, large requests, nonblocking `SSL_read` / `SSL_MODE_AUTO_RETRY`, stream/call/channel ownership, and production vs benchmark boundaries.

## Issues

### REVIEW-20260518-001: production TLS I/O tuning lacks recorded before/after adoption evidence

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC transport domain model reviewer`
- Finding: Production transport changes for TLS write coalescing and OpenSSL read-ahead are present in `ext/grpc/transport.c`, but the corresponding open issues still only describe the plan and completion conditions; they do not record the before measurement, target workload, adoption criteria, after measurement, compatibility verification, or final decision.
- Evidence: `ext/grpc/transport.c:710`, `ext/grpc/transport.c:711`, `ext/grpc/transport.c:932`, `ext/grpc/transport.c:1302`, `docs/issues/open/2026-05-18-github-issue-2-tls-write-coalescing.md`, `docs/issues/open/2026-05-18-github-issue-3-tls-read-buffering.md`
- Expected model: Hotpath transport optimization must keep the benchmark/diagnostic rationale outside production code while recording hypothesis, target workload, before/after measurements, compatibility checks, and adoption or rollback decision in the issue.
- Why it matters: The code-level domain ownership is otherwise clean, but without durable evidence the repository cannot distinguish an accepted production transport invariant from an unvalidated benchmark experiment, especially for persistent TLS connections and nonblocking OpenSSL behavior.
- Recommended fix: Update both issue files with progress, measured before/after results, PHPT/static/integration or smoke verification, large-request observations, and an explicit adoption/rollback decision before closing or committing the production change.
- Fix summary: `GitHub issue #2/#3のbefore観測、ローカルTLS strace after、PHPT、静的解析、主要ベンチsmoke、h2c CPU smoke、採用判断をローカルissueへ追記した。`
- Fix commit: `pending`
- Verification: `docs/issues/open/2026-05-18-github-issue-2-tls-write-coalescing.md` と `docs/issues/open/2026-05-18-github-issue-3-tls-read-buffering.md` に検証結果を記録。
- Notes: The code review found no Blocker/High/Medium protocol-model issue in the inspected paths. `send_pending_h2_frames()` keeps the coalescing flush boundary at one `nghttp2_session_send()` call, large callback payloads bypass the buffer, failed flushes propagate through callers that mark the connection dead, h2c bypasses coalescing through `connection->tls`, and the write buffer is allocated/freed with the `h2_connection` persistent flag.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
