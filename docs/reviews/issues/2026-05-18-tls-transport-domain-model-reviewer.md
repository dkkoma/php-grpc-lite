# TLS transport domain model review 2026-05-18

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/transport.c`
- `bench/run.sh`
- `tools/benchmark/cpu-micro.php`
- `tools/benchmark/spanner-shape.php`
- `docs/issues/closed/2026-05-18-github-issue-2-tls-write-coalescing.md`
- `docs/issues/closed/2026-05-18-github-issue-3-tls-read-buffering.md`

## Reviewer Role

- HTTP/2 / gRPC / TLS transport domain model reviewer

## Review Prompt Summary

- Review current uncommitted TLS write coalescing, OpenSSL read-ahead, and new TLS benchmark suites for nghttp2 send callback semantics, TLS/h2c separation, flush/error/deadline boundaries, persistent allocation lifecycle, SSL_read nonblocking semantics, benchmark vs production boundaries, and issue evidence accuracy.

## Issues

### REVIEW-20260518-001: Closed issue files previously declared `Status: Open`

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC / TLS transport domain model reviewer`
- Finding: The two GitHub issue tracking files are located under `docs/issues/closed/` and include final judgment sections with `Status: Closed`, but their frontmatter still says `Status: Open`.
- Evidence: `docs/issues/closed/2026-05-18-github-issue-2-tls-write-coalescing.md`, `docs/issues/closed/2026-05-18-github-issue-3-tls-read-buffering.md`
- Expected model: An issue's durable lifecycle state should be represented consistently by its path and metadata. Closed issues should have `Status: Closed` in frontmatter so issue scans, future reviewers, and release notes do not treat completed transport work as still open.
- Why it matters: This does not affect runtime behavior, but it violates the repository's issue workflow and weakens the evidence trail for accepting the TLS transport changes.
- Recommended fix: Change both frontmatter values from `Status: Open` to `Status: Closed`.
- Fix summary: `closed issue frontmatterをStatus: Closedへ更新し、path・frontmatter・判断欄の状態を一致させた。`
- Fix commit: `pending`
- Verification: `docs/issues/closed/2026-05-18-github-issue-2-tls-write-coalescing.md` と `docs/issues/closed/2026-05-18-github-issue-3-tls-read-buffering.md` のfrontmatterと判断欄が `Closed` で一致。
- Notes: The measured evidence itself is specific enough for this gate: local TLS strace records write coalescing/read-ahead behavior, and the new `tls-cpu-micro` / `tls-spanner-shape` comparisons include 0.0.4 vs current data.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Accepted Checks

- nghttp2 send callback semantics: callback may buffer bytes and return `length` because `send_pending_h2_frames()` flushes before clearing the send context; send failure is propagated as `NGHTTP2_ERR_CALLBACK_FAILURE` and callers mark the connection dead.
- TLS/h2c separation: write coalescing is scoped to `connection->tls`; h2c keeps direct write semantics.
- Flush/error/deadline boundaries: the flush boundary is one `nghttp2_session_send()` call, and the existing call/setup deadline fields are reused for buffered flushes.
- Persistent allocation lifecycle: `write_buffer` uses `pemalloc(..., connection->persistent)` and `pefree(..., connection->persistent)` with the `h2_connection` owner.
- SSL_read nonblocking semantics: `SSL_set_read_ahead()` is limited to TLS setup and does not add `SSL_MODE_AUTO_RETRY`, so explicit `SSL_ERROR_WANT_READ/WANT_WRITE` poll/deadline control remains intact.
- Benchmark vs production boundaries: TLS suites are benchmark scripts only; production transport does not gain benchmark-specific branches.

## Re-review 2026-05-18

- Result: accepted for the current uncommitted TLS transport and benchmark changes.
- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Latest Accepted Checks

- TLS write coalescing remains scoped to `h2_connection` ownership and `connection->tls`; h2c write path is not converted to buffered writes.
- nghttp2 `send_callback()` buffering is paired with `send_pending_h2_frames()` flush before clearing write context, so callback ownership and failure propagation stay explicit.
- OpenSSL read-ahead is enabled without `SSL_MODE_AUTO_RETRY`, preserving nonblocking `SSL_read` + poll/deadline control.
- `tls-spanner-shape` and `tls-cpu-micro` are benchmark suites only and identify `benchmark.security=tls`; production code does not depend on benchmark-only concepts.
- Closed issue records now have consistent path/frontmatter/final decision state and contain 0.0.4 vs current evidence.
