# deterministic coverage domain review 2026-05-11

## Scope

- `ext/grpc/transport_core.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/unit/test_transport_core.c`
- `ext/grpc/tests/022-error-and-http-validation.phpt`
- `docs/issues/closed/2026-05-11-deterministic-coverage-design.md`

## Reviewer Role

- Domain model / HTTP/2 and gRPC test semantics self reviewer

## Review Prompt Summary

- Review whether deterministic coverage additions assert gRPC/HTTP/2/PHP API semantics rather than current implementation details, and whether pure helper extraction preserves responsibility boundaries.

## Issues

### REVIEW-20260511-001: none

- Severity: `none`
- Status: `Closed`
- Reviewer role: `Domain model / HTTP/2 and gRPC test semantics self reviewer`
- Finding: `none`
- Evidence: `ext/grpc/transport_core.c`, `ext/grpc/tests/unit/test_transport_core.c`, `ext/grpc/tests/022-error-and-http-validation.phpt`
- Expected model: Pure transport configuration and identity helpers can be unit-tested without socket/TLS/nghttp2 side effects; PHPT should assert public gRPC/HTTP semantics such as status code, details, and message count; timing-dependent transport failure should remain out of PHPT until deterministic fault injection exists.
- Why it matters: Coverage work must not freeze incidental implementation flags, read/write counts, or flaky timing behavior as public contract.
- Recommended fix: `none`
- Fix summary: `none`
- Fix commit: `this commit`
- Verification: `./tools/test/check-c-unit.sh`; target PHPT; `./tools/test/check-phpt.sh`; `./tools/test/check-c-coverage.sh`; `./tools/test/check-c-static-analysis.sh`.
- Notes: `transport_core.c` keeps side-effect-free channel/path/limit normalization separate from socket/TLS/nghttp2 lifecycle. Server streaming invalid trailer test intentionally allows one already-delivered message before final invalid status.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
